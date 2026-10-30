#include "EchoHandler.h"
#include "chunkharbor/protocol/FrameCodec.h"
void echoFrames(const chunkharbor::TcpConnection::Ptr& connection, chunkharbor::Buffer& input) {
    using namespace chunkharbor::protocol;
    while (!connection->closed()) {
        auto result = decode(input.readableView());
        if (result.status == DecodeStatus::NeedMore) return;
        if (result.status == DecodeStatus::Error ||
            (result.frame.type != 16 && result.frame.type != 5)) { connection->close(); return; }
        input.consume(result.consumed);
        if (result.frame.type == 16) result.frame.type = 17;
        connection->send(encode(result.frame));
    }
}
