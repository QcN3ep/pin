# Pin

## A header-only, zero-dependency module management library

Pin is a header-only, zero-dependency C++23 module management library. It provides a DI container and a module system for correctly and conveniently handling calls and coordination between different modules in large C++ projects, guaranteeing build safety while offering a high-performance, lock-free, allocation-free hot path.

## Quick start

Pure DI container:

```cpp
#include <pin/pin.hpp>

pin::Registry reg;
reg.bind<ILogger, ConsoleLogger>(pin::Lifetime::Singleton);
reg.bind<IFileSystem>(pin::Lifetime::Singleton).factory([](pin::Resolver &r) {
    return std::make_shared<RealFileSystem>(r.get<ILogger>(), r.get<Config>());
});
reg.bind<ILogger>("file").instance<FileLogger>();
reg.instance<Config>(loadConfig());

pin::Provider provider(std::move(reg));

ILogger &log = provider.ref<ILogger>();
auto file = provider.get<ILogger>("file");
```

Modular assembly:

```cpp
struct RenderModule : pin::Module {
    using Depends = pin::Deps<WindowModule, MemoryModule>;
    static constexpr std::string_view moduleName = "Render";

    void configure(pin::Registry &reg) const;
    void start(pin::Resolver &r);
    void stop() noexcept;
};

pin::Host host;
host.useThreads(4).add<MemoryModule>().add<WindowModule>().add<RenderModule>();

pin::Application app = host.build();
app.run([](pin::Resolver &r) {
    // Main loop; on return, stop in reverse topological order and destroy in reverse order.
});
```

## License

[MIT LICENSE](LICENSE)
