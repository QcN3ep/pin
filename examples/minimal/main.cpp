#include <pin/pin.hpp>

#include <memory>
#include <print>
#include <string>

namespace {

struct Formatter {
    // NOLINTBEGIN(readability-convert-member-functions-to-static)
    [[nodiscard]] std::string decorate(std::string_view text) const {
        return "[" + std::string(text) + "]";
    }
    // NOLINTEND(readability-convert-member-functions-to-static)
};

struct ILogger {
    virtual ~ILogger() = default;
    virtual void info(std::string_view text) const = 0;
};

struct ConsoleLogger : ILogger {
    std::shared_ptr<Formatter> fmt_;
    explicit ConsoleLogger(pin::Resolver &r) : fmt_(r.get<Formatter>()) {}
    void info(std::string_view text) const override {
        std::println("console: {}", fmt_->decorate(text));
    }
};

struct FileLogger : ILogger {
    void info(std::string_view text) const override {
        std::println("file: {}", text);
    }
};

struct Request {
    int seq_ = 0;
};

struct Session {
    int user_ = 0;
};

} // namespace

int runDemo();

int main() { // NOLINT(bugprone-exception-escape)
    try {
        return runDemo();
    } catch (const std::exception &e) {
        std::println("fatal: {}", e.what());
        return 1;
    } catch (...) {
        std::println("fatal: unknown error");
        return 1;
    }
}

int runDemo() {
    pin::Registry reg;
    reg.bind<Formatter>(pin::Lifetime::Singleton);
    reg.bind<ILogger, ConsoleLogger>(pin::Lifetime::Singleton);
    reg.bind<ILogger>("file").instance<FileLogger>();
    reg.bind<Request>(pin::Lifetime::Transient);
    reg.bind<Session>(pin::Lifetime::Scoped);

    pin::Provider provider(std::move(reg));

    provider.ref<ILogger>().info("eager singleton");
    provider.get<ILogger>("file")->info("qualified binding");

    auto r1 = provider.get<Request>();
    auto r2 = provider.get<Request>();
    std::println("transient requests are distinct: {}",
                 static_cast<void *>(r1.get()) !=
                     static_cast<void *>(r2.get()));

    for (int frame = 0; frame < 3; ++frame) {
        pin::Scope frameScope(provider);
        auto &s = frameScope.ref<Session>();
        s.user_ = frame;
        std::println("frame {}: session user = {}", frame,
                     frameScope.ref<Session>().user_);
    }

    std::println("missing service? {}",
                 provider.tryGet<Request>("qualifier") == nullptr);
    return 0;
}
