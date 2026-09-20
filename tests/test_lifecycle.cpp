#include <doctest/doctest.h>
#include <pin/pin.hpp>

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Log {
    std::vector<std::string> events_;
};

struct SharedSvc {
    int value_ = 3;
};

struct CoreModule : pin::Module {
    static constexpr std::string_view moduleName = "Core";
    Log *l_;
    explicit CoreModule(Log &ref) : l_(&ref) {}

    void configure(pin::Registry &reg) const override {
        l_->events_.emplace_back("configure:Core");
        reg.bind<SharedSvc>(pin::Lifetime::Singleton);
    }

    void start(pin::Resolver &r) override {
        l_->events_.emplace_back("start:Core");
        (void)r.ref<SharedSvc>();
    }

    void stop() noexcept override { l_->events_.emplace_back("stop:Core"); }
};

struct WindowModule : pin::Module {
    static constexpr std::string_view moduleName = "Window";
    Log *l_;
    explicit WindowModule(Log &ref) : l_(&ref) {}
    void configure(pin::Registry &) const override {
        l_->events_.emplace_back("configure:Window");
    }
    void start(pin::Resolver &) override {
        l_->events_.emplace_back("start:Window");
    }
    void stop() noexcept override { l_->events_.emplace_back("stop:Window"); }
};

struct RenderModule : pin::Module {
    using Depends = pin::Deps<CoreModule, WindowModule>;
    static constexpr std::string_view moduleName = "Render";
    Log *l_;
    explicit RenderModule(Log &ref) : l_(&ref) {}
    void configure(pin::Registry &) const override {
        l_->events_.emplace_back("configure:Render");
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
    void configure(pin::Registry &) const override {
        l_->events_.emplace_back("configure:Game");
    }
    void start(pin::Resolver &) override {
        l_->events_.emplace_back("start:Game");
    }
    void stop() noexcept override { l_->events_.emplace_back("stop:Game"); }
};

struct MidModule : pin::Module {
    using Depends = pin::Deps<CoreModule>;
    Log *l_;
    explicit MidModule(Log &ref) : l_(&ref) {}
    void start(pin::Resolver &) override {
        l_->events_.emplace_back("start:Mid");
    }
    void stop() noexcept override { l_->events_.emplace_back("stop:Mid"); }
};

struct BoomModule : pin::Module {
    using Depends = pin::Deps<MidModule>;
    explicit BoomModule(Log &) {}
    void start(pin::Resolver &) override { throw std::runtime_error("boom"); }
};

struct LeafModuleA : pin::Module {
    std::atomic<int> *count_;
    explicit LeafModuleA(std::atomic<int> &c) : count_(&c) {}
    void start(pin::Resolver &) override { count_->fetch_add(1); }
};

struct LeafModuleB : pin::Module {
    std::atomic<int> *count_;
    explicit LeafModuleB(std::atomic<int> &c) : count_(&c) {}
    void start(pin::Resolver &) override { count_->fetch_add(1); }
};

struct JoinModule : pin::Module {
    using Depends = pin::Deps<LeafModuleA, LeafModuleB>;
    std::atomic<int> *count_;
    std::atomic<int> *observed_;
    JoinModule(std::atomic<int> &c, std::atomic<int> &o)
        : count_(&c), observed_(&o) {}
    void start(pin::Resolver &) override { observed_->store(count_->load()); }
};

struct CycTwo;
struct CycOne : pin::Module {
    using Depends = pin::Deps<CycTwo>;
};
struct CycTwo : pin::Module {
    using Depends = pin::Deps<CycOne>;
};

struct SelfDep : pin::Module {
    using Depends = pin::Deps<SelfDep>;
};

struct RunThrows : pin::Module {
    Log *l_;
    explicit RunThrows(Log &ref) : l_(&ref) {}
    void stop() noexcept override {
        l_->events_.emplace_back("stop:RunThrows");
    }
};

} // namespace

TEST_CASE("configure and start follow topological order, stop is reverse") {
    Log l;
    {
        pin::Host host;
        host.add<CoreModule>(l)
            .add<WindowModule>(l)
            .add<RenderModule>(l)
            .add<GameModule>(l);

        pin::Application app = host.build();
        app.run([](pin::Resolver &) {});
    }

    std::vector<std::string> gotConfigure;
    std::vector<std::string> gotStart;
    std::vector<std::string> gotStop;
    for (const std::string &e : l.events_) {
        if (e.starts_with("configure:")) {
            gotConfigure.push_back(e);
        } else if (e.starts_with("start:")) {
            gotStart.push_back(e);
        } else {
            gotStop.push_back(e);
        }
    }
    CHECK(gotConfigure ==
          std::vector<std::string>{"configure:Core", "configure:Window",
                                   "configure:Render", "configure:Game"});
    CHECK(gotStart == std::vector<std::string>{"start:Core", "start:Window",
                                               "start:Render", "start:Game"});
    CHECK(gotStop == std::vector<std::string>{"stop:Game", "stop:Render",
                                              "stop:Window", "stop:Core"});
}

TEST_CASE("start receives services registered in configure") {
    Log l;
    pin::Host host;
    host.add<CoreModule>(l);
    pin::Application app = host.build();
    app.run([](pin::Resolver &r) { CHECK(r.ref<SharedSvc>().value_ == 3); });
}

TEST_CASE("missing module dependency is reported at build") {
    Log l;
    pin::Host host;
    host.add<RenderModule>(l);

    try {
        pin::Application app = host.build();
        FAIL("expected ConfigError");
    } catch (const pin::ConfigError &e) {
        const std::string msg = e.what();
        CHECK(msg.find("Render") != std::string::npos);
        CHECK(msg.find("not added") != std::string::npos);
    }
}

TEST_CASE("module dependency cycles report the full path") {
    pin::Host host;
    host.add<CycOne>().add<CycTwo>();

    try {
        pin::Application app = host.build();
        FAIL("expected ConfigError");
    } catch (const pin::ConfigError &e) {
        const std::string msg = e.what();
        CHECK(msg.find("cycle") != std::string::npos);
        CHECK(msg.find("->") != std::string::npos);
    }
}

TEST_CASE("self-dependency is a cycle") {
    pin::Host host;
    host.add<SelfDep>();
    CHECK_THROWS_AS((void)host.build(), pin::ConfigError);
}

TEST_CASE("adding the same module twice is rejected") {
    Log l;
    pin::Host host;
    host.add<CoreModule>(l);
    CHECK_THROWS_AS(host.add<CoreModule>(l), pin::ConfigError);
}

TEST_CASE("start failure rolls back started modules in reverse order") {
    Log l;
    pin::Host host;
    host.add<BoomModule>(l).add<MidModule>(l).add<CoreModule>(l);
    pin::Application app = host.build();

    try {
        app.run([](pin::Resolver &) {});
        FAIL("expected exception");
    } catch (const pin::Error &e) {
        CHECK(std::string(e.what()).find("boom") != std::string::npos);
    }

    std::vector<std::string> stopSequence;
    for (const std::string &e : l.events_) {
        if (e.starts_with("stop:")) {
            stopSequence.push_back(e);
        }
    }
    CHECK(stopSequence == std::vector<std::string>{"stop:Mid", "stop:Core"});
}

TEST_CASE("an exception in run still stops modules") {
    Log l;
    pin::Host host;
    host.add<RunThrows>(l);
    pin::Application app = host.build();

    bool threw = false;
    try {
        app.run(
            [](pin::Resolver &) { throw std::runtime_error("main failed"); });
    } catch (const std::runtime_error &) {
        threw = true;
    }
    CHECK(threw);
    CHECK(std::ranges::find(l.events_, "stop:RunThrows") != l.events_.end());
}

TEST_CASE("parallel start respects level barriers") {
    std::atomic<int> leafCount = 0;
    std::atomic<int> observed = -1;

    pin::Host host;
    host.useThreads(4);
    host.add<JoinModule>(leafCount, observed)
        .add<LeafModuleA>(leafCount)
        .add<LeafModuleB>(leafCount);

    pin::Application app = host.build();
    app.run([](pin::Resolver &) {});

    CHECK(leafCount.load() == 2);
    CHECK(observed.load() == 2);
}

TEST_CASE("modules without declared dependencies form one level") {
    Log l;
    pin::Host host;
    host.add<CoreModule>(l).add<WindowModule>(l);
    pin::Application app = host.build();
    app.run([](pin::Resolver &) {});
    CHECK(l.events_.size() == 6);
}
