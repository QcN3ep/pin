#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "errors.hpp"
#include "type_id.hpp"

namespace pin {
namespace detail {

template <typename T> std::string serviceDebugName(std::string_view qualifier) {
    std::string name = typeName<T>();
    if (!qualifier.empty()) {
        name += ": '" + std::string(qualifier) + "'";
    }
    return name;
}

} // namespace detail

enum class Lifetime : std::uint8_t { Singleton, Scoped, Transient };

// How the caller holds the resolved instance. A borrowed access (ref/tryGet)
// returns a raw reference whose lifetime is owned by the container, so it is
// only valid for singleton and scoped services.
enum class Access : std::uint8_t { Owned, Borrowed };

struct ServiceKey {
    TypeId type_;
    std::string qualifier_;

    constexpr std::strong_ordering
    operator<=>(const ServiceKey &) const = default;
};

class Resolver {
  public:
    virtual ~Resolver() = default;
    Resolver(const Resolver &) = delete;
    Resolver &operator=(const Resolver &) = delete;

    template <typename T>
    [[nodiscard]] std::shared_ptr<T> get(std::string_view qualifier = {}) {
        std::shared_ptr<void> raw =
            resolveService(typeId<T>(), qualifier, Access::Owned);
        if (raw == nullptr) {
            throw ResolveError("pin: service '" + describe<T>(qualifier) +
                               "' is not registered");
        }
        return std::static_pointer_cast<T>(std::move(raw));
    }

    // Returns nullptr if the service is not registered. Throws ResolveError
    // for transient services: the instance would be destroyed on return.
    template <typename T>
    [[nodiscard]] T *tryGet(std::string_view qualifier = {}) {
        std::shared_ptr<void> raw =
            resolveService(typeId<T>(), qualifier, Access::Borrowed);
        return raw == nullptr ? nullptr : static_cast<T *>(raw.get());
    }

    // Throws ResolveError if the service is not registered or is transient.
    template <typename T>
    [[nodiscard]] T &ref(std::string_view qualifier = {}) {
        std::shared_ptr<void> raw =
            resolveService(typeId<T>(), qualifier, Access::Borrowed);
        if (raw == nullptr) {
            throw ResolveError("pin: service '" + describe<T>(qualifier) +
                               "' is not registered");
        }
        return *static_cast<T *>(raw.get());
    }

  protected:
    Resolver() = default;

    virtual std::shared_ptr<void>
    resolveService(TypeId type, std::string_view qualifier, Access access) = 0;

    template <typename T>
    static std::string describe(std::string_view qualifier) {
        return detail::serviceDebugName<T>(qualifier);
    }

    static ResolveError transientBorrowError(const std::string &debugName) {
        return ResolveError("pin: transient service '" + debugName +
                            "' cannot be borrowed with ref<T>()/tryGet<T>(); "
                            "use get<T>() to take ownership");
    }
};

} // namespace pin
