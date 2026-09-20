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
        std::shared_ptr<void> raw = resolveService(typeId<T>(), qualifier);
        if (raw == nullptr) {
            throw ResolveError("pin: service '" + describe<T>(qualifier) +
                               "' is not registered");
        }
        return std::static_pointer_cast<T>(std::move(raw));
    }

    template <typename T>
    [[nodiscard]] T *tryGet(std::string_view qualifier = {}) {
        std::shared_ptr<void> raw = resolveService(typeId<T>(), qualifier);
        return raw == nullptr ? nullptr : static_cast<T *>(raw.get());
    }

    template <typename T>
    [[nodiscard]] T &ref(std::string_view qualifier = {}) {
        return *get<T>(qualifier);
    }

  protected:
    Resolver() = default;

    virtual std::shared_ptr<void>
    resolveService(TypeId type, std::string_view qualifier) = 0;

    template <typename T>
    static std::string describe(std::string_view qualifier) {
        return detail::serviceDebugName<T>(qualifier);
    }
};

} // namespace pin
