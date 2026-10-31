#pragma once
#include "chunkharbor/protocol/Handshake.h"

namespace chunkharbor::client {
class HandshakeExchange {
public:
    explicit HandshakeExchange(protocol::Hello offer = {}, std::uint64_t requestId = 1);
    const protocol::Frame& request() const noexcept { return request_; }
    void accept(const protocol::Frame& response);
    bool ready() const noexcept { return state_ == State::Ready; }
    const protocol::HelloAck& ack() const;
private:
    enum class State { Pending, Ready, Failed };
    protocol::Hello offer_;
    protocol::Frame request_;
    protocol::HelloAck ack_;
    State state_ = State::Pending;
};
}
