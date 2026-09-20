#pragma once

#include <atomic>
#include <compare>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <typeinfo>

#ifdef __GNUG__
#include <cxxabi.h>
#endif

namespace pin {
namespace detail {

inline std::uint64_t allocateTypeId() noexcept {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

inline std::string demangle(const char *name) {
#ifdef __GNUG__
    int status = 0;
    char *demangled = abi::__cxa_demangle(name, nullptr, nullptr, &status);
    if (status == 0 && demangled != nullptr) {
        std::string result(demangled);
        std::free(demangled);
        return result;
    }
#endif
    return name;
}

} // namespace detail

class TypeId {
  public:
    constexpr TypeId() noexcept = default;

    template <typename T> static TypeId of() noexcept {
        static const std::uint64_t kId = detail::allocateTypeId();
        return TypeId(kId);
    }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return id_; }

    constexpr std::strong_ordering
    operator<=>(const TypeId &) const noexcept = default;

  private:
    constexpr explicit TypeId(std::uint64_t id) noexcept : id_(id) {}

    std::uint64_t id_ = 0;
};

template <typename T> [[nodiscard]] inline TypeId typeId() noexcept {
    return TypeId::of<T>();
}

template <typename T> [[nodiscard]] inline std::string typeName() {
#ifndef PIN_NO_RTTI
    return detail::demangle(typeid(T).name());
#else
    return "#" + std::to_string(typeId<T>().value());
#endif
}

} // namespace pin
