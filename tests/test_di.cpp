#include <doctest/doctest.h>
#include <pin/pin.hpp>

#include <memory>
#include <string>
#include <vector>

namespace {

int ctorCount = 0;

struct Plain {
    int value_ = 42;
};

struct Counting {
    Counting() { ++ctorCount; }
};

struct ILogger {
    virtual ~ILogger() = default;
    [[nodiscard]] virtual std::string name() const = 0;
};

struct ConsoleLogger : ILogger {
    [[nodiscard]] std::string name() const override { return "console"; }
};

struct Formatter {
    [[nodiscard]] static std::string decorate(std::string_view s) {
        return "[" + std::string(s) + "]";
    }
};

struct NeedsFormatter {
    std::shared_ptr<Formatter> fmt_;
    explicit NeedsFormatter(pin::Resolver &r) : fmt_(r.get<Formatter>()) {}
};

struct CycA {};
struct CycB {};

struct DtorLog {
    std::vector<std::string> *out_{};
    std::string name_;
    ~DtorLog() { out_->push_back(name_); }
};

void resetCounters() { ctorCount = 0; }

} // namespace

TEST_CASE("singleton is eager and stable") {
    resetCounters();
    pin::Registry reg;
    reg.bind<Counting>(pin::Lifetime::Singleton);
    CHECK(ctorCount == 0);

    pin::Provider p(std::move(reg));
    CHECK(ctorCount == 1);

    auto a = p.get<Counting>();
    auto b = p.get<Counting>();
    CHECK(a.get() == b.get());
    CHECK(ctorCount == 1);
}

TEST_CASE("transient creates a new instance per resolve") {
    resetCounters();
    pin::Registry reg;
    reg.bind<Counting>(pin::Lifetime::Transient);

    pin::Provider p(std::move(reg));
    auto a = p.get<Counting>();
    auto b = p.get<Counting>();
    CHECK(a.get() != b.get());
    CHECK(ctorCount == 2);
}

TEST_CASE("later registration wins for the same key") {
    pin::Registry reg;
    reg.bind<ILogger, ConsoleLogger>();
    reg.bind<ILogger>().factory([](pin::Resolver &) {
        struct FileLogger : ILogger {
            [[nodiscard]] std::string name() const override { return "file"; }
        };
        return std::make_shared<FileLogger>();
    });

    pin::Provider p(std::move(reg));
    CHECK(p.get<ILogger>()->name() == "file");
}

TEST_CASE("qualifiers keep registrations distinct") {
    pin::Registry reg;
    reg.bind<ILogger, ConsoleLogger>(pin::Lifetime::Singleton, "console");
    reg.bind<ILogger>("file").factory([](pin::Resolver &) {
        struct FileLogger : ILogger {
            [[nodiscard]] std::string name() const override { return "file"; }
        };
        return std::make_shared<FileLogger>();
    });

    pin::Provider p(std::move(reg));
    CHECK(p.get<ILogger>("console")->name() == "console");
    CHECK(p.get<ILogger>("file")->name() == "file");
    CHECK(p.ref<ILogger>("console").name() == "console");
}

TEST_CASE("instance registration accepts values and shared pointers") {
    pin::Registry reg;
    reg.instance<Plain>(Plain{.value_ = 7});
    reg.instance<ILogger>(std::make_shared<ConsoleLogger>());

    pin::Provider p(std::move(reg));
    CHECK(p.ref<Plain>().value_ == 7);
    CHECK(p.get<ILogger>()->name() == "console");
}

TEST_CASE("binding an interface constructs the implementation") {
    pin::Registry reg;
    reg.bind<ILogger, ConsoleLogger>();

    pin::Provider p(std::move(reg));
    CHECK(p.get<ILogger>()->name() == "console");
}

TEST_CASE("resolver-injected constructor wiring") {
    pin::Registry reg;
    reg.bind<Formatter>(pin::Lifetime::Singleton);
    reg.bind<NeedsFormatter>(pin::Lifetime::Singleton);

    pin::Provider p(std::move(reg));
    auto nf = p.get<NeedsFormatter>();
    CHECK(nf->fmt_ == p.get<Formatter>());
}

TEST_CASE("missing service throws with the type name") {
    pin::Registry reg;
    reg.bind<Plain>();

    pin::Provider p(std::move(reg));
    CHECK(p.tryGet<Formatter>() == nullptr);
    try {
        (void)p.get<Formatter>();
        FAIL("expected ResolveError");
    } catch (const pin::ResolveError &e) {
        CHECK(std::string(e.what()).find("Formatter") != std::string::npos);
    }
}

TEST_CASE("lazy singleton builds on first use exactly once") {
    resetCounters();
    pin::Registry reg;
    reg.bind<Counting>().lazy().factory(
        [](pin::Resolver &) { return std::make_shared<Counting>(); });

    pin::Provider p(std::move(reg));
    CHECK(ctorCount == 0);
    (void)p.ref<Counting>();
    CHECK(ctorCount == 1);
    (void)p.get<Counting>();
    CHECK(ctorCount == 1);
}

TEST_CASE("ref fast path returns the singleton") {
    pin::Registry reg;
    reg.bind<Plain>();

    pin::Provider p(std::move(reg));
    auto &a = p.ref<Plain>();
    auto &b = p.ref<Plain>();
    CHECK(&a == &b);
    CHECK(&a == p.get<Plain>().get());
}

TEST_CASE("ref on a non-singleton is rejected") {
    pin::Registry reg;
    reg.bind<Plain>(pin::Lifetime::Transient);

    pin::Provider p(std::move(reg));
    CHECK_THROWS_AS((void)p.ref<Plain>(), pin::ResolveError);
}

TEST_CASE("abstract service without factory fails at build") {
    pin::Registry reg;
    reg.bind<ILogger>();

    CHECK_THROWS_AS(pin::Provider(std::move(reg)), pin::ConfigError);
}

TEST_CASE("lazy non-singleton fails at build") {
    pin::Registry reg;
    reg.bind<Plain>(pin::Lifetime::Transient).lazy();

    CHECK_THROWS_AS(pin::Provider(std::move(reg)), pin::ConfigError);
}

TEST_CASE("scoped service cannot be resolved from the root") {
    pin::Registry reg;
    reg.bind<Plain>(pin::Lifetime::Scoped);

    pin::Provider p(std::move(reg));
    CHECK_THROWS_AS((void)p.get<Plain>(), pin::ResolveError);
}

TEST_CASE("service dependency cycles are reported") {
    pin::Registry reg;
    reg.bind<CycA>().factory([](pin::Resolver &r) {
        (void)r.get<CycB>();
        return std::make_shared<CycA>();
    });
    reg.bind<CycB>().factory([](pin::Resolver &r) {
        (void)r.get<CycA>();
        return std::make_shared<CycB>();
    });

    try {
        pin::Provider p(std::move(reg));
        FAIL("expected ResolveError");
    } catch (const pin::ResolveError &e) {
        const std::string msg = e.what();
        CHECK(msg.find("cycle") != std::string::npos);
        CHECK(msg.find("->") != std::string::npos);
    }
}

TEST_CASE("factory returning nullptr is rejected") {
    pin::Registry reg;
    reg.bind<Plain>().factory(
        [](pin::Resolver &) { return std::shared_ptr<Plain>(); });

    CHECK_THROWS_AS(pin::Provider(std::move(reg)), pin::ResolveError);
}

TEST_CASE("singletons are destroyed in reverse creation order") {
    std::vector<std::string> order;
    {
        pin::Registry reg;
        reg.bind<DtorLog>().factory([&order](pin::Resolver &) {
            return std::make_shared<DtorLog>(&order, "a");
        });
        reg.bind<DtorLog>("b").factory([&order](pin::Resolver &r) {
            (void)r.get<DtorLog>();
            return std::make_shared<DtorLog>(&order, "b");
        });

        pin::Provider p(std::move(reg));
    }
    CHECK(order == std::vector<std::string>{"b", "a"});
}

TEST_CASE("type ids are unique and stable per type") {
    CHECK(pin::typeId<Plain>() == pin::typeId<Plain>());
    CHECK(pin::typeId<Plain>() != pin::typeId<Formatter>());
    CHECK(pin::typeId<Plain>().value() != 0);
}
