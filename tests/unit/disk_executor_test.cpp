#include "chunkharbor/concurrency/DiskExecutor.h"
#include "chunkharbor/net/EventLoop.h"
#include <future>
#include <stdexcept>
#include <thread>

int main() {
    using namespace chunkharbor;
    EventLoop loop;
    DiskExecutor executor(loop, 1, 2);
    std::promise<void> started, release;
    auto gate = release.get_future().share();
    int callbacks = 0;
    bool correct = true;
    auto finish = [&](DiskExecutor::Result result) {
        correct = correct && loop.isInLoopThread();
        if (result.error) {
            try { std::rethrow_exception(result.error); }
            catch (const std::runtime_error&) {} catch (...) { correct = false; }
        } else correct = correct && std::any_cast<int>(result.value) == 42;
        if (++callbacks == 2) loop.quit();
    };
    if (executor.trySubmit([&]() -> std::any {
        correct = correct && !loop.isInLoopThread();
        started.set_value(); gate.wait(); return 42;
    }, finish) != DiskExecutor::Submit::Accepted) return 1;
    started.get_future().wait();
    if (executor.trySubmit([]() -> std::any { throw std::runtime_error("disk failure"); }, finish) != DiskExecutor::Submit::Accepted) return 2;
    if (executor.trySubmit([] { return std::any(0); }, finish) != DiskExecutor::Submit::Full) return 3;
    release.set_value();
    loop.run(); executor.stop();
    if (!correct || callbacks != 2 || executor.outstanding() != 0) return 4;
    if (executor.trySubmit([] { return std::any{}; }, finish) != DiskExecutor::Submit::Stopped) return 5;
    DiskExecutor drain(loop, 2, 8);
    int done = 0;
    for (int i = 0; i < 8; ++i)
        if (drain.trySubmit([] { return std::any(7); }, [&](auto r) {
            if (std::any_cast<int>(r.value) != 7 || !loop.isInLoopThread()) throw std::runtime_error("result");
            ++done;
        }) != DiskExecutor::Submit::Accepted) return 6;
    drain.stop(); drain.stop();
    if (done != 8) return 7;
    // Destructor joins workers and cancels undelivered callbacks.
    { DiskExecutor cancelled(loop, 1, 1);
      cancelled.trySubmit([] { return std::any{}; }, [&](auto) { ++done; }); }
    if (done != 8) return 8;
    DiskExecutor throwing(loop, 1, 2);
    throwing.trySubmit([] { return std::any{}; }, [](auto) { throw std::runtime_error("callback"); });
    throwing.trySubmit([] { return std::any{}; }, [&](auto) { ++done; });
    bool caught = false;
    try { throwing.stop(); } catch (const std::runtime_error&) { caught = true; }
    if (!caught || done != 9 || throwing.outstanding() != 0) return 9;
    EventLoop fullLoop(1); fullLoop.queueInLoop([] {});
    DiskExecutor independent(fullLoop, 1, 1);
    independent.trySubmit([] { return std::any{}; }, [&](auto) { ++done; });
    independent.stop(); // Completion transport must work even when pending_ is full.
    if (done != 10) return 10;
    DiskExecutor concurrent(loop, 2, 64);
    std::atomic<int> accepted{0};
    int delivered = 0;
    std::vector<std::thread> producers;
    for (int i = 0; i < 4; ++i) producers.emplace_back([&] {
        for (int j = 0; j < 8; ++j)
            if (concurrent.trySubmit([] { return std::any(1); }, [&](auto) {
                if (++delivered == 32) loop.quit();
            }) == DiskExecutor::Submit::Accepted) ++accepted;
    });
    for (auto& producer : producers) producer.join();
    loop.run(); concurrent.stop();
    return accepted == 32 && delivered == 32 ? 0 : 11;
}
