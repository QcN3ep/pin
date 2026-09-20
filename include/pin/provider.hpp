#pragma once

#include <algorithm>
#include <memory>
#include <mutex>
#include <numeric>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "errors.hpp"
#include "registry.hpp"
#include "resolver.hpp"
#include "type_id.hpp"

namespace pin {

class Provider;

namespace detail {

struct ResolveFrame {
    const void *provider_;
    const std::string *debugName_;
};

inline thread_local std::vector<ResolveFrame> resolveStack;

class ResolveGuard {
  public:
    ResolveGuard(const void *prov, const std::string &name)
        : provider_(prov), name_(&name) {
        for (const ResolveFrame &f : resolveStack) {
            if (f.provider_ == provider_ && f.debugName_ == name_) {
                throw ResolveError(cycleMessage());
            }
        }
        resolveStack.push_back(
            ResolveFrame{.provider_ = provider_, .debugName_ = name_});
    }

    ~ResolveGuard() { resolveStack.pop_back(); }

    ResolveGuard(const ResolveGuard &) = delete;
    ResolveGuard &operator=(const ResolveGuard &) = delete;

  private:
    [[nodiscard]] std::string cycleMessage() const {
        std::string msg = "pin: service dependency cycle detected: ";
        for (std::size_t i = 0; i < resolveStack.size(); ++i) {
            const ResolveFrame &first = resolveStack[i];
            if (first.provider_ == provider_ && first.debugName_ == name_) {
                for (std::size_t j = i; j < resolveStack.size(); ++j) {
                    msg += *resolveStack[j].debugName_ +
                           (j + 1 < resolveStack.size() ? " -> " : "");
                }
                msg += " -> " + *name_;
                break;
            }
        }
        return msg;
    }

    const void *provider_;
    const std::string *name_;
};

struct SlotNode {
    virtual ~SlotNode() = default;
    virtual void clear() noexcept = 0;
};

template <typename T> struct FastSlot : SlotNode {
    Provider *owner_ = nullptr;
    T *value_ = nullptr;
    bool linked_ = false;

    void clear() noexcept override {
        owner_ = nullptr;
        value_ = nullptr;
        linked_ = false;
    }

    ~FastSlot() override;
};

} // namespace detail

class Provider : public Resolver {
  public:
    struct Descriptor {
        ServiceKey key_;
        std::string debugName_;
        std::move_only_function<std::shared_ptr<void>(Resolver &) const>
            factory_;
        Lifetime lifetime_ = Lifetime::Singleton;
        bool lazy_ = false;
        std::size_t order_ = 0;
    };

    static constexpr std::size_t
        NOT_FOUND = // NOLINT(readability-identifier-naming)
        static_cast<std::size_t>(-1);

    explicit Provider(Registry &&reg) { freeze(std::move(reg)); }

    Provider(const Provider &) = delete;
    Provider &operator=(const Provider &) = delete;

    ~Provider() override {
        {
            std::scoped_lock lock(slotsMutex_);
            for (detail::SlotNode *n : slots_) {
                n->clear();
            }
            slots_.clear();
        }
        for (std::size_t i : std::ranges::reverse_view(creationOrder_)) {
            instances_[i].reset();
        }
    }

    template <typename T>
    [[nodiscard]] T &ref(std::string_view qualifier = {}) {
        if (!qualifier.empty()) {
            return Resolver::ref<T>(qualifier);
        }
        static thread_local detail::FastSlot<T> slot;
        if (slot.owner_ == this) {
            return *slot.value_;
        }
        T *raw = &resolveRef<T>(qualifier);
        if (slot.linked_) {
            slot.owner_->unlinkSlot(slot);
        }
        slot.owner_ = this;
        slot.value_ = raw;
        registerSlot(slot);
        slot.linked_ = true;
        return *slot.value_;
    }

    [[nodiscard]] std::size_t findDescriptor(TypeId type,
                                             std::string_view qualifier) {
        std::size_t lo = 0;
        std::size_t hi = descs_.size();
        while (lo < hi) {
            std::size_t mid = lo + ((hi - lo) / 2);
            if (keyLess(descs_[mid].key_, type, qualifier)) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo < descs_.size()) {
            const Descriptor &d = descs_[lo];
            if (d.key_.type_ == type &&
                std::string_view(d.key_.qualifier_) == qualifier) {
                return lo;
            }
        }
        return NOT_FOUND;
    }

    [[nodiscard]] const Descriptor &descriptor(std::size_t i) {
        return descs_[i];
    }

    [[nodiscard]] std::shared_ptr<void> resolveDescriptor(std::size_t i) {
        const Descriptor &d = descs_[i];
        switch (d.lifetime_) {
        case Lifetime::Singleton:
            return resolveSingleton(i);
        case Lifetime::Scoped:
            throw ResolveError("pin: scoped service '" + d.debugName_ +
                               "' cannot be resolved from the root provider");
        case Lifetime::Transient: {
            detail::ResolveGuard guard(this, d.debugName_);
            return d.factory_(*this);
        }
        }
        std::unreachable();
    }

    [[nodiscard]] std::shared_ptr<void> resolveSingleton(std::size_t i) {
        // The guard must precede call_once: a service cycle re-entering
        // call_once on the same thread would deadlock.
        const Descriptor &d = descs_[i];
        detail::ResolveGuard guard(this, d.debugName_);
        std::call_once(onceFlags_[i], [this, i, &d] {
            std::shared_ptr<void> inst = d.factory_(*this);
            if (inst == nullptr) {
                throw ResolveError("pin: factory for '" + d.debugName_ +
                                   "' returned nullptr");
            }
            recordCreation(i);
            instances_[i] = std::move(inst);
        });
        return instances_[i];
    }

    void registerSlot(detail::SlotNode &n) {
        std::scoped_lock lock(slotsMutex_);
        slots_.push_back(&n);
    }

    void unlinkSlot(detail::SlotNode &n) {
        std::scoped_lock lock(slotsMutex_);
        std::erase(slots_, &n);
    }

  protected:
    std::shared_ptr<void> resolveService(TypeId type,
                                         std::string_view qualifier) override {
        std::size_t i = findDescriptor(type, qualifier);
        return i == NOT_FOUND ? nullptr : resolveDescriptor(i);
    }

  private:
    template <typename T>
    [[nodiscard]] T &resolveRef(std::string_view qualifier) {
        std::size_t i = findDescriptor(typeId<T>(), qualifier);
        if (i == NOT_FOUND) {
            throw ResolveError("pin: service '" + describe<T>(qualifier) +
                               "' is not registered");
        }
        if (descs_[i].lifetime_ != Lifetime::Singleton) {
            throw ResolveError("pin: ref<T>() requires a singleton service, '" +
                               descs_[i].debugName_ + "' is not one");
        }
        return *static_cast<T *>(resolveSingleton(i).get());
    }

    void freeze(Registry &&reg) {
        std::vector<std::size_t> byKey(reg.regs_.size());
        std::ranges::iota(byKey, std::size_t{0});
        std::ranges::stable_sort(byKey, [&](std::size_t a, std::size_t b) {
            return reg.regs_[a].key_ < reg.regs_[b].key_;
        });

        std::vector<std::size_t> keep;
        keep.reserve(reg.regs_.size());
        for (std::size_t i = 0; i < byKey.size(); ++i) {
            if (i + 1 == byKey.size() ||
                reg.regs_[byKey[i]].key_ != reg.regs_[byKey[i + 1]].key_) {
                keep.push_back(byKey[i]);
            }
        }

        descs_.reserve(keep.size());
        for (std::size_t idx : keep) {
            Registry::Registration &r = reg.regs_[idx];
            if (r.factory_ == nullptr) {
                throw ConfigError("pin: binding for '" + r.debugName_ +
                                  "' has no factory");
            }
            if (r.lazy_ && r.lifetime_ != Lifetime::Singleton) {
                throw ConfigError("pin: binding for '" + r.debugName_ +
                                  "' is lazy but not a singleton");
            }
            descs_.push_back(Descriptor{
                .key_ = std::move(r.key_),
                .debugName_ = std::move(r.debugName_),
                .factory_ = std::move(r.factory_),
                .lifetime_ = r.lifetime_,
                .lazy_ = r.lazy_,
                .order_ = r.order_,
            });
        }
        onceFlags_ = std::make_unique<std::once_flag[]>(descs_.size());
        instances_.resize(descs_.size());

        std::vector<std::size_t> eager;
        eager.reserve(descs_.size());
        for (std::size_t i = 0; i < descs_.size(); ++i) {
            if (descs_[i].lifetime_ == Lifetime::Singleton &&
                !descs_[i].lazy_) {
                eager.push_back(i);
            }
        }
        std::ranges::sort(eager, [&](std::size_t a, std::size_t b) {
            return descs_[a].order_ < descs_[b].order_;
        });
        for (std::size_t i : eager) {
            (void)resolveSingleton(i);
        }
    }

    void recordCreation(std::size_t i) {
        while (creationLock_.test_and_set(std::memory_order_acquire)) {
        }
        creationOrder_.push_back(i);
        creationLock_.clear(std::memory_order_release);
    }

    static bool keyLess(const ServiceKey &k, TypeId type,
                        std::string_view qualifier) {
        if (k.type_ != type) {
            return k.type_ < type;
        }
        return k.qualifier_ < qualifier;
    }

    std::vector<Descriptor> descs_;
    std::unique_ptr<std::once_flag[]> onceFlags_;
    std::vector<std::shared_ptr<void>> instances_;
    std::vector<std::size_t> creationOrder_;
    std::atomic_flag creationLock_;
    std::mutex slotsMutex_;
    std::vector<detail::SlotNode *> slots_;
};

namespace detail {

template <typename T> FastSlot<T>::~FastSlot() {
    if (linked_ && owner_ != nullptr) {
        owner_->unlinkSlot(*this);
    }
}

} // namespace detail

} // namespace pin
