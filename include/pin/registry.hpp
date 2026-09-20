#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "errors.hpp"
#include "resolver.hpp"
#include "type_id.hpp"

namespace pin {
namespace detail {

template <typename T>
concept ServiceCtor =
    std::constructible_from<T, Resolver &> || std::default_initializable<T>;

template <typename Service, typename Impl>
std::move_only_function<std::shared_ptr<void>(Resolver &) const>
defaultFactory() {
    if constexpr (std::constructible_from<Impl, Resolver &>) {
        return [](Resolver &r) -> std::shared_ptr<void> {
            return std::static_pointer_cast<Service>(std::make_shared<Impl>(r));
        };
    } else {
        static_assert(
            std::default_initializable<Impl>,
            "pin: Impl must be constructible from pin::Resolver& or be "
            "default-constructible");
        return [](Resolver &) -> std::shared_ptr<void> {
            return std::static_pointer_cast<Service>(std::make_shared<Impl>());
        };
    }
}

} // namespace detail

class Provider;

class Registry {
  public:
    Registry() = default;
    Registry(const Registry &) = delete;
    Registry &operator=(const Registry &) = delete;

    template <typename Service> class Binding {
      public:
        template <typename F>
            requires std::invocable<F &, Resolver &>
        Binding &factory(F &&fct) {
            reg_->regs_[index_].factory_ =
                [fct = std::forward<F>(fct)](
                    Resolver &r) -> std::shared_ptr<void> {
                return std::static_pointer_cast<Service>(std::invoke(fct, r));
            };
            return *this;
        }

        Binding &lifetime(Lifetime l) {
            reg_->regs_[index_].lifetime_ = l;
            return *this;
        }

        Binding &lazy() {
            reg_->regs_[index_].lazy_ = true;
            return *this;
        }

        Binding &instance(std::shared_ptr<Service> obj) {
            reg_->regs_[index_].lazy_ = false;
            reg_->regs_[index_].factory_ =
                [obj = std::move(obj)](Resolver &) -> std::shared_ptr<void> {
                return obj;
            };
            return *this;
        }

        template <typename Impl = Service, typename... Args>
            requires std::derived_from<Impl, Service>
        Binding &instance(Args &&...ctorArgs) {
            return instance(std::shared_ptr<Service>(
                std::make_shared<Impl>(std::forward<Args>(ctorArgs)...)));
        }

      private:
        friend class Registry;
        Binding(Registry *reg, std::size_t index) : reg_(reg), index_(index) {}

        Registry *reg_;
        std::size_t index_;
    };

    template <typename Service>
    Binding<Service> bind(std::string_view qualifier = {}) {
        Binding<Service> b = openBinding<Service>(qualifier);
        if constexpr (detail::ServiceCtor<Service>) {
            regs_.back().factory_ = detail::defaultFactory<Service, Service>();
        }
        return b;
    }

    template <typename Service>
    Binding<Service> bind(Lifetime l, std::string_view qualifier = {}) {
        Binding<Service> b = bind<Service>(qualifier);
        return b.lifetime(l);
    }

    template <typename Service, typename Impl>
        requires std::derived_from<Impl, Service> && detail::ServiceCtor<Impl>
    Binding<Service> bind(Lifetime l = Lifetime::Singleton,
                          std::string_view qualifier = {}) {
        Binding<Service> b = openBinding<Service>(qualifier);
        regs_.back().lifetime_ = l;
        regs_.back().factory_ = detail::defaultFactory<Service, Impl>();
        return b;
    }

    template <typename Service>
    Binding<Service> instance(std::shared_ptr<Service> obj,
                              std::string_view qualifier = {}) {
        return bind<Service>(qualifier).instance(std::move(obj));
    }

    template <typename Service>
    Binding<Service> instance(Service &&value,
                              std::string_view qualifier = {}) {
        return bind<Service>(qualifier).instance(
            std::make_shared<Service>(std::forward<Service>(value)));
    }

  private:
    friend class Provider;

    struct Registration {
        ServiceKey key_;
        std::string debugName_;
        std::move_only_function<std::shared_ptr<void>(Resolver &) const>
            factory_;
        Lifetime lifetime_ = Lifetime::Singleton;
        bool lazy_ = false;
        std::size_t order_ = 0;
    };

    template <typename Service>
    Binding<Service> openBinding(std::string_view qualifier) {
        ServiceKey key{typeId<Service>(), std::string(qualifier)};
        regs_.push_back(Registration{
            .key_ = std::move(key),
            .debugName_ = detail::serviceDebugName<Service>(qualifier),
            .factory_ = nullptr,
            .lifetime_ = Lifetime::Singleton,
            .order_ = regs_.size(),
        });
        return Binding<Service>{this, regs_.size() - 1};
    }

    std::vector<Registration> regs_;
};

} // namespace pin
