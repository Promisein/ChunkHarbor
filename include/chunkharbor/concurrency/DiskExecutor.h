#pragma once
#include "chunkharbor/common/UniqueFd.h"
#include "chunkharbor/net/Channel.h"
#include <any>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace chunkharbor {
class EventLoop;
// Construct/destroy/stop on the loop thread, before the EventLoop is destroyed.
// Workers must be finite and must not wait for a loop callback to finish.
class DiskExecutor {
public:
    struct Result { std::any value; std::exception_ptr error; };
    enum class Submit { Accepted, Full, Stopped };
    using Work = std::function<std::any()>;
    using Completion = std::function<void(Result)>;
    DiskExecutor(EventLoop& loop, std::size_t workers, std::size_t capacity);
    ~DiskExecutor();
    DiskExecutor(const DiskExecutor&) = delete;
    DiskExecutor& operator=(const DiskExecutor&) = delete;
    Submit trySubmit(Work work, Completion completion);
    // Gracefully join and dispatch remaining results, including after loop.run returns.
    // May block; call only during shutdown, never from a completion/worker.
    void stop();
    std::size_t outstanding() const;
private:
    struct Job { Work work; Completion completion; };
    struct Completed { Completion callback; Result result; };
    void worker();
    void join();
    void dispatch();
    EventLoop& loop_;
    const std::size_t capacity_;
    UniqueFd wakeFd_;
    Channel channel_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Job> jobs_;
    std::deque<Completed> completed_;
    std::vector<std::thread> threads_;
    std::size_t outstanding_ = 0;
    bool stopping_ = false;
    bool dispatching_ = false;
};
}
