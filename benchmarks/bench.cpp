#include <benchmark/benchmark.h>
#include <pin/pin.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <utility>

namespace {

struct Hot {
    int v_ = 0;
};

struct FrameCtx {
    int frame_ = 0;
};

struct PerCall {
    int x_ = 1;
};

template <std::size_t N> struct Nop {};

constexpr std::size_t MAX_NOPS = 512;

template <std::size_t I> void regNop(pin::Registry &reg) {
    reg.bind<Nop<I>>(pin::Lifetime::Transient);
}

template <std::size_t I> void *findNop(pin::Provider &p) {
    return p.tryGet<Nop<I>>();
}

template <std::size_t... Is>
constexpr auto makeRegFns(std::index_sequence<Is...>) {
    return std::array<void (*)(pin::Registry &), sizeof...(Is)>{&regNop<Is>...};
}

template <std::size_t... Is>
constexpr auto makeFindFns(std::index_sequence<Is...>) {
    return std::array<void *(*)(pin::Provider &), sizeof...(Is)>{
        &findNop<Is>...};
}

constexpr auto REG_FNS = makeRegFns(std::make_index_sequence<MAX_NOPS>{});
constexpr auto FIND_FNS = makeFindFns(std::make_index_sequence<MAX_NOPS>{});

pin::Provider makeProviderScaled(std::size_t nopCount) {
    pin::Registry reg;
    reg.bind<Hot>(pin::Lifetime::Singleton);
    reg.bind<FrameCtx>(pin::Lifetime::Scoped);
    reg.bind<PerCall>(pin::Lifetime::Transient);
    for (std::size_t i = 0; i < nopCount && i < MAX_NOPS; ++i) {
        REG_FNS[i](reg);
    }
    return pin::Provider(std::move(reg));
}

void bmRefHot(benchmark::State &state) {
    pin::Provider p = makeProviderScaled(48);
    (void)p.ref<Hot>();
    for (auto _ : state) {
        benchmark::DoNotOptimize(p.ref<Hot>());
    }
}
BENCHMARK(bmRefHot);

void bmGetSharedPtr(benchmark::State &state) {
    pin::Provider p = makeProviderScaled(48);
    for (auto _ : state) {
        benchmark::DoNotOptimize(p.get<Hot>());
    }
}
BENCHMARK(bmGetSharedPtr);

void bmTransientGet(benchmark::State &state) {
    pin::Provider p = makeProviderScaled(48);
    for (auto _ : state) {
        benchmark::DoNotOptimize(p.get<PerCall>());
    }
}
BENCHMARK(bmTransientGet);

void bmFindScaling(benchmark::State &state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    pin::Provider p = makeProviderScaled(count);
    const auto find = FIND_FNS[count / 2];
    for (auto _ : state) {
        benchmark::DoNotOptimize(find(p));
    }
}
BENCHMARK(bmFindScaling)->Arg(8)->Arg(64)->Arg(512);

void bmScopeCachedResolve(benchmark::State &state) {
    pin::Provider p = makeProviderScaled(48);
    pin::Scope s(p);
    for (auto _ : state) {
        benchmark::DoNotOptimize(s.get<FrameCtx>());
    }
}
BENCHMARK(bmScopeCachedResolve);

void bmScopeCreateDestroy(benchmark::State &state) {
    pin::Provider p = makeProviderScaled(48);
    for (auto _ : state) {
        pin::Scope s(p);
        benchmark::DoNotOptimize(&s);
    }
}
BENCHMARK(bmScopeCreateDestroy);

void bmScopeFull(benchmark::State &state) {
    pin::Provider p = makeProviderScaled(48);
    for (auto _ : state) {
        pin::Scope s(p);
        benchmark::DoNotOptimize(s.get<FrameCtx>());
    }
}
BENCHMARK(bmScopeFull);

void bmRefConcurrent(benchmark::State &state) {
    static pin::Provider p = makeProviderScaled(64);
    (void)p.ref<Hot>();
    for (auto _ : state) {
        benchmark::DoNotOptimize(p.ref<Hot>());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(bmRefConcurrent)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->UseRealTime();

void bmTransientConcurrent(benchmark::State &state) {
    static pin::Provider p = makeProviderScaled(64);
    for (auto _ : state) {
        benchmark::DoNotOptimize(p.get<PerCall>());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(bmTransientConcurrent)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->UseRealTime();

void bmBaselineRawDeref(benchmark::State &state) {
    auto obj = std::make_shared<Hot>();
    Hot *ptr = obj.get();
    for (auto _ : state) {
        ptr->v_ += 1;
        benchmark::DoNotOptimize(ptr);
    }
}
BENCHMARK(bmBaselineRawDeref);

void bmBaselineSharedPtrCopy(benchmark::State &state) {
    auto obj = std::make_shared<Hot>();
    for (auto _ : state) {
        std::shared_ptr<Hot> copy = obj;
        benchmark::DoNotOptimize(copy);
    }
}
BENCHMARK(bmBaselineSharedPtrCopy);

template <int N> struct Chain : pin::Module {
    using Depends = pin::Deps<Chain<N - 1>>;
};

template <> struct Chain<0> : pin::Module {};

void bmHostLifecycle(benchmark::State &state) {
    for (auto _ : state) {
        pin::Host host;
        host.add<Chain<0>>().add<Chain<1>>().add<Chain<2>>().add<Chain<3>>();
        pin::Application app = host.build();
        app.run([](pin::Resolver &) {});
        benchmark::DoNotOptimize(&app);
    }
}
BENCHMARK(bmHostLifecycle);

} // namespace
