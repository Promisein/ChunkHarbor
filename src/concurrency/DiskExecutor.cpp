#include "chunkharbor/concurrency/DiskExecutor.h"
#include "chunkharbor/common/Error.h"
#include "chunkharbor/net/EventLoop.h"
#include <cerrno>
#include <stdexcept>
#include <sys/eventfd.h>
#include <unistd.h>

namespace chunkharbor {
DiskExecutor::DiskExecutor(EventLoop& loop, std::size_t workers, std::size_t capacity)
    : loop_(loop), capacity_(capacity), wakeFd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
      channel_(loop, wakeFd_.get()) {
    loop_.assertInLoopThread();
    if (!workers || workers > 64 || !capacity || capacity > 65536)
        throw std::invalid_argument("invalid DiskExecutor worker/capacity limits");
    if (!wakeFd_) throw systemError(errno, "DiskExecutor eventfd");
    channel_.setReadCallback([this] { dispatch(); });
    try {
        for (std::size_t i = 0; i < workers; ++i) threads_.emplace_back([this] { worker(); });
        channel_.enableReading();
    } catch (...) { join(); throw; }
}
DiskExecutor::~DiskExecutor() {
    if (!loop_.isInLoopThread() || dispatching_) std::terminate();
    channel_.remove();
    join(); // Workers cannot access the object or FD after this returns.
    // Remaining callbacks are cancelled: destructors never call application code.
}
DiskExecutor::Submit DiskExecutor::trySubmit(Work work, Completion completion) {
    if (!work || !completion) throw std::invalid_argument("empty DiskExecutor callback");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return Submit::Stopped;
        if (outstanding_ == capacity_) return Submit::Full;
        jobs_.push_back({std::move(work), std::move(completion)});
        ++outstanding_;
    }
    ready_.notify_one(); return Submit::Accepted;
}
std::size_t DiskExecutor::outstanding() const {
    std::lock_guard<std::mutex> lock(mutex_); return outstanding_;
}
void DiskExecutor::worker() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (jobs_.empty()) return;
            job = std::move(jobs_.front()); jobs_.pop_front();
        }
        Result result;
        try { result.value = job.work(); } catch (...) { result.error = std::current_exception(); }
        job.work = {}; // Release large work captures before retaining its result.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            completed_.push_back({std::move(job.completion), std::move(result)});
        }
        const std::uint64_t one = 1; ssize_t n;
        do { n = ::write(wakeFd_.get(), &one, sizeof(one)); } while (n < 0 && errno == EINTR);
        if (n < 0 && errno != EAGAIN) std::terminate(); // FD stays live until all workers join.
    }
}
void DiskExecutor::join() {
    { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
    ready_.notify_all();
    for (auto& thread : threads_) if (thread.joinable()) thread.join();
}
void DiskExecutor::stop() {
    loop_.assertInLoopThread();
    if (dispatching_) throw std::logic_error("stop inside a completion is forbidden");
    join(); dispatch(); channel_.remove();
}
void DiskExecutor::dispatch() {
    loop_.assertInLoopThread();
    std::uint64_t ticks;
    for (;;) {
        const auto n = ::read(wakeFd_.get(), &ticks, sizeof(ticks));
        if (n == sizeof(ticks) || (n < 0 && errno == EINTR)) continue;
        if (n < 0 && errno == EAGAIN) break;
        throw systemError(errno, "DiskExecutor eventfd read");
    }
    std::size_t count;
    { std::lock_guard<std::mutex> lock(mutex_); count = completed_.size(); }
    std::exception_ptr firstError;
    dispatching_ = true;
    for (std::size_t i = 0; i < count; ++i) {
        Completed completed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            completed = std::move(completed_.front()); completed_.pop_front();
        }
        try { completed.callback(std::move(completed.result)); }
        catch (...) { if (!firstError) firstError = std::current_exception(); }
        { std::lock_guard<std::mutex> lock(mutex_); --outstanding_; }
    }
    dispatching_ = false;
    if (firstError) std::rethrow_exception(firstError);
}
}
