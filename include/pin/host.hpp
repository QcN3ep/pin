#pragma once

#include <algorithm>
#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "errors.hpp"
#include "module.hpp"
#include "provider.hpp"
#include "registry.hpp"
#include "resolver.hpp"
#include "scope.hpp"
#include "type_id.hpp"

namespace pin {

class Application;

class Host {
  public:
    Host() = default;
    Host(const Host &) = delete;
    Host &operator=(const Host &) = delete;

    template <ModuleType M, typename... Args> Host &add(Args &&...ctorArgs) {
        for (const Entry &e : modules_) {
            if (e.id_ == typeId<M>()) {
                throw ConfigError("pin: module '" + e.name_ +
                                  "' was added twice");
            }
        }
        modules_.push_back(Entry{
            .id_ = typeId<M>(),
            .name_ = detail::moduleNameOf<M>(),
            .deps_ = detail::moduleDepsOf<M>(),
            .depIdx_ = {},
            .mod_ = std::make_unique<M>(std::forward<Args>(ctorArgs)...),
        });
        return *this;
    }

    Host &useThreads(unsigned n) {
        threads_ = n == 0 ? 1u : n;
        return *this;
    }

    [[nodiscard]] Application build();

  private:
    struct Entry {
        TypeId id_;
        std::string name_;
        std::vector<std::pair<TypeId, std::string>> deps_;
        std::vector<std::uint32_t> depIdx_;
        std::unique_ptr<Module> mod_;
    };

    std::vector<Entry> modules_;
    unsigned threads_ = 1;
};

class Application {
  public:
    Application(const Application &) = delete;
    Application &operator=(const Application &) = delete;

    ~Application() {
        if (started_ && !stopped_) {
            stopAll();
        }
    }

    template <typename F> void run(F &&main) {
        startAll();
        try {
            main(provider_);
        } catch (...) {
            stopAll();
            throw;
        }
        stopAll();
    }

    [[nodiscard]] Provider &provider() noexcept { return provider_; }
    [[nodiscard]] Resolver &resolver() noexcept { return provider_; }

    template <typename T> [[nodiscard]] T &ref() { return provider_.ref<T>(); }

    template <typename T> [[nodiscard]] std::shared_ptr<T> get() {
        return provider_.get<T>();
    }

    template <typename T> [[nodiscard]] T *tryGet() {
        return provider_.tryGet<T>();
    }

    [[nodiscard]] Scope scope() { return Scope(provider_); }

  private:
    friend class Host;

    struct ModEntry {
        std::string name_;
        std::unique_ptr<Module> mod_;
        bool started_ = false;
    };

    Application(Registry &&reg, std::vector<ModEntry> modules,
                std::vector<std::vector<std::uint32_t>> levels,
                unsigned threads)
        : modules_(std::move(modules)), levels_(std::move(levels)),
          provider_(std::move(reg)), threads_(threads) {}

    void startAll() {
        if (started_) {
            throw Error("pin: application already started");
        }
        for (const std::vector<std::uint32_t> &level : levels_) {
            std::atomic<std::size_t> next{0};
            std::mutex errMutex;
            std::vector<std::exception_ptr> errors;

            const auto worker = [&] {
                for (;;) {
                    std::size_t i =
                        next.fetch_add(1, std::memory_order_relaxed);
                    if (i >= level.size()) {
                        break;
                    }
                    ModEntry &e = modules_[level[i]];
                    try {
                        e.mod_->start(provider_);
                        e.started_ = true;
                    } catch (...) {
                        const std::scoped_lock lock(errMutex);
                        errors.push_back(std::current_exception());
                    }
                }
            };
            runWorkers(worker, level.size());

            if (!errors.empty()) {
                rollback();
                throw Error(joinErrors(errors));
            }
        }
        started_ = true;
    }

    void stopAll() {
        if (!started_ || stopped_) {
            return;
        }
        for (auto &level : std::views::reverse(levels_)) {
            std::atomic<std::size_t> next{0};
            const auto worker = [&] {
                for (;;) {
                    std::size_t i =
                        next.fetch_add(1, std::memory_order_relaxed);
                    if (i >= level.size()) {
                        break;
                    }
                    ModEntry &e = modules_[level[level.size() - 1 - i]];
                    e.mod_->stop();
                    e.started_ = false;
                }
            };
            runWorkers(worker, level.size());
        }
        stopped_ = true;
        started_ = false;
    }

    void rollback() {
        for (auto &module : std::views::reverse(modules_)) {
            if (!module.started_) {
                continue;
            }
            module.mod_->stop();
            module.started_ = false;
        }
    }

    template <typename W> void runWorkers(W &&worker, std::size_t count) {
        if (threads_ <= 1 || count <= 1) {
            worker();
            return;
        }
        const std::size_t n = std::min<std::size_t>(threads_, count);
        std::vector<std::jthread> pool;
        pool.reserve(n - 1);
        for (std::size_t i = 0; i + 1 < n; ++i) {
            pool.emplace_back(worker);
        }
        worker();
    }

    static std::string
    joinErrors(const std::vector<std::exception_ptr> &errors) {
        std::string first;
        std::size_t count = 0;
        for (const std::exception_ptr &p : errors) {
            try {
                std::rethrow_exception(p);
            } catch (const std::exception &e) {
                if (count == 0) {
                    first = e.what();
                }
                ++count;
            } catch (...) {
                if (count == 0) {
                    first = "unknown error";
                }
                ++count;
            }
        }
        std::string msg = "pin: module start failed: " + first;
        if (count > 1) {
            msg += " (+" + std::to_string(count - 1) + " more errors)";
        }
        return msg;
    }

    std::vector<ModEntry> modules_;
    std::vector<std::vector<std::uint32_t>> levels_;
    Provider provider_;
    unsigned threads_;
    bool started_ = false;
    bool stopped_ = false;
};

inline Application Host::build() {
    const std::size_t n = modules_.size();

    for (Entry &e : modules_) {
        for (const auto &[DepId, DepName] : e.deps_) {
            const auto it = std::ranges::find_if(
                modules_, [&](const Entry &m) { return m.id_ == DepId; });
            if (it == modules_.end()) {
                throw ConfigError("pin: module '" + e.name_ +
                                  "' depends on module '" + DepName +
                                  "' which was not added to the host");
            }
            e.depIdx_.push_back(
                static_cast<std::uint32_t>(it - modules_.begin()));
        }
        std::ranges::sort(e.depIdx_);
        const auto [First, Last] = std::ranges::unique(e.depIdx_);
        e.depIdx_.erase(First, Last);
    }

    std::vector<std::vector<std::uint32_t>> dependents(n);
    std::vector<std::uint32_t> inDeg(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        inDeg[i] = static_cast<std::uint32_t>(modules_[i].depIdx_.size());
        for (std::uint32_t d : modules_[i].depIdx_) {
            dependents[d].push_back(static_cast<std::uint32_t>(i));
        }
    }

    std::vector<std::vector<std::uint32_t>> levels;
    std::vector<bool> remaining(n, true);
    std::vector<std::uint32_t> current;
    for (std::size_t i = 0; i < n; ++i) {
        if (inDeg[i] == 0) {
            current.push_back(static_cast<std::uint32_t>(i));
        }
    }

    std::size_t processed = 0;
    while (!current.empty()) {
        for (std::uint32_t m : current) {
            remaining[m] = false;
        }
        processed += current.size();
        levels.push_back(current);
        std::vector<std::uint32_t> next;
        for (std::uint32_t m : current) {
            for (std::uint32_t dependent : dependents[m]) {
                if (--inDeg[dependent] == 0) {
                    next.push_back(dependent);
                }
            }
        }
        current = std::move(next);
    }

    if (processed != n) {
        std::vector<std::uint32_t> path;
        std::vector<bool> onPath(n, false);
        std::uint32_t cur = 0;
        while (!remaining[cur]) {
            ++cur;
        }
        for (;;) {
            if (onPath[cur]) {
                const auto it = std::ranges::find(path, cur);
                path.assign(it, path.end());
                break;
            }
            onPath[cur] = true;
            path.push_back(cur);
            std::uint32_t next = n;
            for (std::uint32_t d : modules_[cur].depIdx_) {
                if (remaining[d]) {
                    next = d;
                    break;
                }
            }
            cur = next;
        }
        std::string msg = "pin: module dependency cycle detected: ";
        for (std::size_t i = 0; i < path.size(); ++i) {
            msg +=
                modules_[path[i]].name_ + (i + 1 < path.size() ? " -> " : "");
        }
        msg += " -> " + modules_[path[0]].name_;
        throw ConfigError(msg);
    }

    Registry reg;
    std::vector<Application::ModEntry> mods;
    std::vector<std::uint32_t> flatIdx(n);
    mods.reserve(n);
    for (const std::vector<std::uint32_t> &level : levels) {
        for (std::uint32_t i : level) {
            Entry &e = modules_[i];
            e.mod_->configure(reg);
            flatIdx[i] = static_cast<std::uint32_t>(mods.size());
            mods.push_back(Application::ModEntry{
                .name_ = std::move(e.name_),
                .mod_ = std::move(e.mod_),
            });
        }
    }

    std::vector<std::vector<std::uint32_t>> flatLevels;
    flatLevels.reserve(levels.size());
    for (const std::vector<std::uint32_t> &level : levels) {
        std::vector<std::uint32_t> flat;
        flat.reserve(level.size());
        for (std::uint32_t i : level) {
            flat.push_back(flatIdx[i]);
        }
        flatLevels.push_back(std::move(flat));
    }

    return {std::move(reg), std::move(mods), std::move(flatLevels), threads_};
}

} // namespace pin
