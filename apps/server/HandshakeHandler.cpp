#include "HandshakeHandler.h"
#include "chunkharbor/protocol/Handshake.h"

chunkharbor::TcpConnection::MessageCallback makeHandshakeHandler() {
    return [ready = false](const chunkharbor::TcpConnection::Ptr& connection, chunkharbor::Buffer& input) mutable {
        using namespace chunkharbor::protocol;
        while (!connection->closed()) {
            auto decoded = decode(input.readableView());
            if (decoded.status == DecodeStatus::NeedMore) return;
            if (decoded.status == DecodeStatus::Error) { connection->close(); return; }
            input.consume(decoded.consumed);
            const auto& request = decoded.frame;
            try {
                if (!request.requestId) throw MessageError("MALFORMED_MESSAGE", "request_id must be nonzero");
                if (ready) {
                    if (request.type == 1) throw MessageError("ALREADY_HANDSHAKEN", "HELLO is only allowed once");
                    throw MessageError("UNSUPPORTED_OPERATION", "No application operation is implemented");
                }
                if (request.type != 1) throw MessageError("HANDSHAKE_REQUIRED", "Expected HELLO before application requests");
                auto ack = negotiateHello(decodeHello(request.payload));
                connection->send(encode({2, request.requestId, encodeHelloAck(ack)}));
                ready = true;
            } catch (const MessageError& error) {
                connection->send(encode({15, request.requestId, encodeHandshakeError({error.code, error.what()})}));
                connection->shutdownAfterWrite(); return;
            }
        }
    };
}
