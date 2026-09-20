#include <doctest/doctest.h>
#include <pin/pin.hpp>

#include <memory>
#include <string>
#include <vector>

namespace {

int scopedCtorCount = 0;
int transientCtorCount = 0;

struct Session {
    int id_ = 0;
    Session() { ++scopedCtorCount; }
};

struct Request {
    int seq_ = 0;
    Request() { ++transientCtorCount; }
};

struct Shared {
    int value_ = 5;
};

struct DtorLog {
    std::vector<std::string> *out_ = nullptr;
    std::string name_;
    ~DtorLog() {
        if (out_ != nullptr) {
            out_->push_back(name_);
        }
    }
};

void resetCounters() {
    scopedCtorCount = 0;
    transientCtorCount = 0;
}

} // namespace

TEST_CASE("scoped services are per-scope singletons") {
    resetCounters();
    pin::Registry reg;
    reg.bind<Session>(pin::Lifetime::Scoped);
    reg.bind<Shared>(pin::Lifetime::Singleton);

    pin::Provider p(std::move(reg));

    pin::Scope s1(p);
    CHECK(s1.get<Session>() == s1.get<Session>());
    CHECK(scopedCtorCount == 1);

    pin::Scope s2(p);
    CHECK(s2.get<Session>() != s1.get<Session>());
    CHECK(scopedCtorCount == 2);
}

TEST_CASE("singletons are shared between root and scopes") {
    pin::Registry reg;
    reg.bind<Shared>(pin::Lifetime::Singleton);

    pin::Provider p(std::move(reg));
    pin::Scope s(p);
    CHECK(&s.ref<Shared>() == &p.ref<Shared>());
}

TEST_CASE("transients are new instances inside a scope") {
    resetCounters();
    pin::Registry reg;
    reg.bind<Request>(pin::Lifetime::Transient);

    pin::Provider p(std::move(reg));
    pin::Scope s(p);
    CHECK(s.get<Request>() != s.get<Request>());
    CHECK(transientCtorCount == 2);
}

TEST_CASE("missing service in a scope returns nullptr via tryGet") {
    pin::Registry reg;
    reg.bind<Shared>();

    pin::Provider p(std::move(reg));
    pin::Scope s(p);
    CHECK(s.tryGet<Session>() == nullptr);
    CHECK_THROWS_AS((void)s.get<Session>(), pin::ResolveError);
}

TEST_CASE("scoped chain resolves through the scope") {
    resetCounters();
    pin::Registry reg;
    reg.bind<Session>(pin::Lifetime::Scoped);
    reg.bind<DtorLog>()
        .lifetime(pin::Lifetime::Scoped)
        .factory([](pin::Resolver &r) {
            (void)r.get<Session>();
            return std::make_shared<DtorLog>();
        });

    std::vector<std::string> order;
    {
        pin::Provider p(std::move(reg));
        pin::Scope s(p);
        (void)s.get<DtorLog>();
        CHECK(scopedCtorCount == 1);
    }
    CHECK(order.empty());
}

TEST_CASE("scope destroys scoped services in reverse creation order") {
    std::vector<std::string> order;
    pin::Registry reg;
    reg.bind<DtorLog>()
        .lifetime(pin::Lifetime::Scoped)
        .factory([&order](pin::Resolver &) {
            return std::make_shared<DtorLog>(&order, "first");
        });
    reg.bind<DtorLog>("second")
        .lifetime(pin::Lifetime::Scoped)
        .factory([&order](pin::Resolver &r) {
            (void)r.get<DtorLog>();
            return std::make_shared<DtorLog>(&order, "second");
        });

    pin::Provider p(std::move(reg));
    {
        pin::Scope s(p);
        (void)s.get<DtorLog>("second");
    }
    CHECK(order == std::vector<std::string>{"second", "first"});
}

TEST_CASE("scope factory can resolve scoped services through the resolver") {
    resetCounters();
    pin::Registry reg;
    reg.bind<Session>(pin::Lifetime::Scoped);

    pin::Provider p(std::move(reg));
    pin::Scope s(p);
    std::shared_ptr<Session> a = s.get<Session>();
    CHECK(a != nullptr);
    CHECK(scopedCtorCount == 1);
}

TEST_CASE("scope is movable") {
    pin::Registry reg;
    reg.bind<Shared>();

    pin::Provider p(std::move(reg));
    pin::Scope outer(p);
    pin::Scope inner = std::move(outer);
    CHECK(inner.get<Shared>() == p.get<Shared>());
}
