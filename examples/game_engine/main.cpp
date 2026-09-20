#include <pin/pin.hpp>

#include <atomic>
#include <memory>
#include <print>

namespace {

struct Log {
    std::vector<std::string> events_;
};

struct GpuDevice {
    std::atomic<int> drawCalls_ = 0;
};

struct Window {
    int width_ = 1280;
    int height_ = 720;
};

struct Renderer {
    GpuDevice *gpu_ = nullptr;
    explicit Renderer(pin::Resolver &r) : gpu_(&r.ref<GpuDevice>()) {}
    void draw() const {
        gpu_->drawCalls_.fetch_add(1, std::memory_order_relaxed);
    }
};

struct FrameStats {
    int frame_ = 0;
};

struct MemoryModule : pin::Module {
    static constexpr std::string_view moduleName = "Memory";
    Log *l_;
    explicit MemoryModule(Log &ref) : l_(&ref) {}
    void start(pin::Resolver &) override {
        l_->events_.emplace_back("start:Memory");
    }
    void stop() noexcept override { l_->events_.emplace_back("stop:Memory"); }
};

struct WindowModule : pin::Module {
    using Depends = pin::Deps<MemoryModule>;
    static constexpr std::string_view moduleName = "Window";
    Log *l_;
    explicit WindowModule(Log &ref) : l_(&ref) {}
    void configure(pin::Registry &reg) const override {
        l_->events_.emplace_back("configure:Window");
        reg.bind<Window>(pin::Lifetime::Singleton);
    }
    void start(pin::Resolver &) override {
        l_->events_.emplace_back("start:Window");
    }
    void stop() noexcept override { l_->events_.emplace_back("stop:Window"); }
};

struct RenderModule : pin::Module {
    using Depends = pin::Deps<WindowModule>;
    static constexpr std::string_view moduleName = "Render";
    Log *l_;
    explicit RenderModule(Log &ref) : l_(&ref) {}
    void configure(pin::Registry &reg) const override {
        l_->events_.emplace_back("configure:Render");
        reg.bind<GpuDevice>(pin::Lifetime::Singleton);
        reg.bind<Renderer>(pin::Lifetime::Singleton);
    }
    void start(pin::Resolver &) override {
        l_->events_.emplace_back("start:Render");
    }
    void stop() noexcept override { l_->events_.emplace_back("stop:Render"); }
};

struct GameModule : pin::Module {
    using Depends = pin::Deps<RenderModule>;
    static constexpr std::string_view moduleName = "Game";
    Log *l_;
    explicit GameModule(Log &ref) : l_(&ref) {}
    void configure(pin::Registry &reg) const override {
        l_->events_.emplace_back("configure:Game");
        reg.bind<FrameStats>(pin::Lifetime::Scoped);
    }
    void start(pin::Resolver &) override {
        l_->events_.emplace_back("start:Game");
    }
    void stop() noexcept override { l_->events_.emplace_back("stop:Game"); }
};

} // namespace

int runEngine();

int main() { // NOLINT(bugprone-exception-escape)
    try {
        return runEngine();
    } catch (const std::exception &e) {
        std::println("fatal: {}", e.what());
        return 1;
    } catch (...) {
        std::println("fatal: unknown error");
        return 1;
    }
}

int runEngine() {
    Log l;

    pin::Host host;
    host.useThreads(4);
    host.add<GameModule>(l)
        .add<RenderModule>(l)
        .add<WindowModule>(l)
        .add<MemoryModule>(l);

    pin::Application app = host.build();

    app.run([&](pin::Resolver &r) {
        for (const std::string &e : l.events_) {
            std::println("{}", e);
        }
        std::println("--- main loop ---");

        const Window &w = r.ref<Window>();
        std::println("window {}x{}", w.width_, w.height_);

        for (int frame = 0; frame < 3; ++frame) {
            pin::Scope frameScope = app.scope();
            auto &stats = frameScope.ref<FrameStats>();
            stats.frame_ = frame;
            app.ref<Renderer>().draw();
            std::println("frame {} done, stats scoped to this frame",
                         stats.frame_);
        }
        std::println("draw calls: {}", r.ref<GpuDevice>().drawCalls_.load());
    });

    std::println("--- shutdown ---");
    for (const std::string &e : l.events_) {
        if (e.starts_with("stop:")) {
            std::println("{}", e);
        }
    }
    return 0;
}
