#pragma once

#include <concepts>
#include <string>
#include <utility>
#include <vector>

#include "registry.hpp"
#include "resolver.hpp"
#include "type_id.hpp"

namespace pin {

template <typename... Modules> struct Deps {
    static std::vector<std::pair<TypeId, std::string>> info() {
        return {std::pair{typeId<Modules>(), typeName<Modules>()}...};
    }
};

class Module {
  public:
    virtual ~Module() = default;
    Module(const Module &) = delete;
    Module &operator=(const Module &) = delete;

    virtual void configure(Registry &) const {}
    virtual void start(Resolver &) {}
    virtual void stop() noexcept {}

  protected:
    Module() = default;
};

namespace detail {

template <typename T>
concept HasDepends = requires {
    typename T::Depends;
    T::Depends::info();
};

template <typename T>
concept HasModuleName = requires {
    { T::moduleName } -> std::convertible_to<std::string_view>;
};

template <typename T> std::string moduleNameOf() {
    if constexpr (HasModuleName<T>) {
        return std::string(T::moduleName);
    } else {
        return typeName<T>();
    }
}

template <typename T>
std::vector<std::pair<TypeId, std::string>> moduleDepsOf() {
    if constexpr (HasDepends<T>) {
        return T::Depends::info();
    } else {
        return {};
    }
}

} // namespace detail

template <typename T>
concept ModuleType = std::derived_from<T, Module>;

} // namespace pin
