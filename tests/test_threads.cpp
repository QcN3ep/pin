#include <doctest/doctest.h>
#include <pin/pin.hpp>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace {

struct LazyHeavy {
    static std::atomic<int> ctorCount;
    LazyHeavy() { ctorCount.fetch_add(1, std::memory_order_relaxed); }
};
std::atomic<int> LazyHeavy::ctorCount{0};

struct Tok {
    static std::atomic<int> total;
    Tok() { total.fetch_add(1, std::memory_order_relaxed); }
};
std::atomic<int> Tok::total{0};

struct Work {
    int payload_ = 1;
};

struct FrameCtx {
    int frame_ = 0;
};

constexpr int THREADS = 8;
constexpr int ITERS = 20000;

} // namespace

TEST_CASE("concurrent resolution of a lazy singleton constructs it once") {
    LazyHeavy::ctorCount.store(0);
    pin::Registry reg;
    reg.bind<LazyHeavy>().lazy().factory(
        [](pin::Resolver &) { return std::make_shared<LazyHeavy>(); });

    pin::Provider p(std::move(reg));
    CHECK(LazyHeavy::ctorCount.load() == 0);

    std::vector<std::thread> pool;
    pool.reserve(THREADS);
    std::atomic<bool> failed{false};
    for (int t = 0; t < THREADS; ++t) {
        pool.emplace_back([&] {
            LazyHeavy *seen = &p.ref<LazyHeavy>();
            for (int i = 0; i < ITERS; ++i) {
                if (&p.ref<LazyHeavy>() != seen ||
                    p.get<LazyHeavy>().get() != seen) {
                    failed.store(true);
                }
            }
        });
    }
    for (std::thread &t : pool) {
        t.join();
    }

    CHECK(!failed.load());
    CHECK(LazyHeavy::ctorCount.load() == 1);
}

TEST_CASE("concurrent transient resolution counts every instance") {
    Tok::total.store(0);
    pin::Registry reg;
    reg.bind<Tok>(pin::Lifetime::Transient).factory([](pin::Resolver &) {
        return std::make_shared<Tok>();
    });

    pin::Provider p(std::move(reg));
    std::vector<std::thread> pool;
    pool.reserve(THREADS);
    for (int t = 0; t < THREADS; ++t) {
        pool.emplace_back([&] {
            for (int i = 0; i < ITERS / 10; ++i) {
                auto ptr = p.get<Tok>();
                if (ptr == nullptr) {
                    FAIL("transient resolution returned nullptr");
                }
            }
        });
    }
    for (std::thread &t : pool) {
        t.join();
    }

    CHECK(Tok::total.load() == THREADS * (ITERS / 10));
}

TEST_CASE("thread-confined scopes resolve independently") {
    pin::Registry reg;
    reg.bind<FrameCtx>(pin::Lifetime::Scoped);
    reg.bind<Work>(pin::Lifetime::Singleton);

    pin::Provider p(std::move(reg));
    Work &rootWork = p.ref<Work>();

    std::vector<std::thread> pool;
    pool.reserve(THREADS);
    std::atomic<bool> failed{false};
    for (int t = 0; t < THREADS; ++t) {
        pool.emplace_back([&] {
            pin::Scope s(p);
            auto *first = s.tryGet<FrameCtx>();
            for (int i = 0; i < 1000; ++i) {
                if (s.tryGet<FrameCtx>() != first) {
                    failed.store(true);
                }
                if (&s.ref<Work>() != &rootWork) {
                    failed.store(true);
                }
            }
        });
    }
    for (std::thread &t : pool) {
        t.join();
    }

    CHECK(!failed.load());
}

TEST_CASE("fast slot reuse across two providers stays consistent") {
    pin::Registry regA;
    regA.bind<Work>(pin::Lifetime::Singleton);
    pin::Registry regB;
    regB.bind<Work>(pin::Lifetime::Singleton);

    pin::Provider a(std::move(regA));
    pin::Provider b(std::move(regB));
    pin::Scope sc(a);

    std::atomic<bool> failed{false};
    std::vector<std::thread> pool;
    pool.reserve(THREADS);
    for (int t = 0; t < 4; ++t) {
        pool.emplace_back([&] {
            for (int i = 0; i < 1000; ++i) {
                if (&a.ref<Work>() == &b.ref<Work>()) {
                    failed.store(true);
                }
                if (&sc.ref<Work>() != &a.ref<Work>()) {
                    failed.store(true);
                }
            }
        });
    }
    for (std::thread &t : pool) {
        t.join();
    }

    CHECK(!failed.load());
}
