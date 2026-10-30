#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace chunkharbor {

class EventLoop;

// Borrows the FD. The owner must keep this object alive through event dispatch.
class Channel {
public:
    Channel(EventLoop& loop, int fd) noexcept;
    ~Channel();
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    int fd() const noexcept { return fd_; }
    std::uint32_t events() const noexcept { return events_; }
    bool registered() const noexcept { return registered_; }
    void tie(const std::shared_ptr<void>& owner) { owner_ = owner; tied_ = true; }
    void setInterest(bool read, bool write);
    void setReadCallback(std::function<void()> callback) { readCallback_ = std::move(callback); }
    void setWriteCallback(std::function<void()> callback) { writeCallback_ = std::move(callback); }
    void setErrorCallback(std::function<void()> callback) { errorCallback_ = std::move(callback); }
    void enableReading();
    void enableWriting();
    void disableAll();
    void remove();
    void handleEvent(std::uint32_t revents);

private:
    friend class EpollPoller;
    void update();
    EventLoop& loop_;
    int fd_;
    std::uint32_t events_ = 0;
    bool registered_ = false;
    bool tied_ = false;
    std::weak_ptr<void> owner_;
    std::function<void()> readCallback_;
    std::function<void()> writeCallback_;
    std::function<void()> errorCallback_;
};

} // namespace chunkharbor
