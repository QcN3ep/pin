#pragma once

#include <utility>
#include <vector>

#include "provider.hpp"
#include "resolver.hpp"

namespace pin {

class Scope : public Resolver {
  public:
    explicit Scope(Provider &root) : root_(&root) {}

    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
    Scope(Scope &&other) noexcept
        : root_(other.root_), scoped_(std::move(other.scoped_)) {}
    Scope &operator=(Scope &&) = delete;

    ~Scope() override {
        for (auto &it : std::views::reverse(scoped_)) {
            it.second.reset();
        }
    }

  private:
    std::shared_ptr<void> resolveService(TypeId type,
                                         std::string_view qualifier) override {
        std::size_t i = root_->findDescriptor(type, qualifier);
        if (i == Provider::NOT_FOUND) {
            return nullptr;
        }
        const Provider::Descriptor &d = root_->descriptor(i);
        switch (d.lifetime_) {
        case Lifetime::Singleton:
            return root_->resolveDescriptor(i);
        case Lifetime::Scoped:
            for (auto &[Idx, Inst] : scoped_) {
                if (Idx == i) {
                    return Inst;
                }
            }
            {
                detail::ResolveGuard guard(root_, d.debugName_);
                std::shared_ptr<void> inst = d.factory_(*this);
                if (inst == nullptr) {
                    throw ResolveError("pin: factory for '" + d.debugName_ +
                                       "' returned nullptr");
                }
                scoped_.emplace_back(i, std::move(inst));
            }
            return scoped_.back().second;
        case Lifetime::Transient: {
            detail::ResolveGuard guard(root_, d.debugName_);
            return d.factory_(*this);
        }
        }
        std::unreachable();
    }

    Provider *root_;
    std::vector<std::pair<std::size_t, std::shared_ptr<void>>> scoped_;
};

} // namespace pin
