#include "chunkharbor/client_core/HandshakeExchange.h"
#include <algorithm>
namespace chunkharbor::client {
using namespace protocol;
HandshakeExchange::HandshakeExchange(Hello offer, std::uint64_t requestId) : offer_(std::move(offer)) {
    if (!requestId) throw MessageError("MALFORMED_MESSAGE", "Handshake request_id must be nonzero");
    request_ = {1, requestId, encodeHello(offer_)};
}
void HandshakeExchange::accept(const Frame& response) {
    if (state_ != State::Pending) throw MessageError("MALFORMED_MESSAGE", "Handshake is already terminal");
    state_ = State::Failed;
    if (response.requestId != request_.requestId) throw MessageError("MALFORMED_MESSAGE", "Mismatched request_id");
    if (response.type == 15) {
        const auto error = decodeHandshakeError(response.payload); throw MessageError(error.code, error.message);
    }
    if (response.type != 2) throw MessageError("MALFORMED_MESSAGE", "Expected HELLO_ACK");
    auto ack = decodeHelloAck(response.payload);
    if (ack.version != 1 || std::find(offer_.versions.begin(), offer_.versions.end(), ack.version) == offer_.versions.end())
        throw MessageError("UNSUPPORTED_VERSION", "Server selected an unsupported version");
    for (const auto& capability : ack.capabilities)
        if (std::find(offer_.capabilities.begin(), offer_.capabilities.end(), capability) == offer_.capabilities.end())
            throw MessageError("MALFORMED_MESSAGE", "Server selected an unoffered capability");
    ack_ = std::move(ack); state_ = State::Ready;
}
const HelloAck& HandshakeExchange::ack() const {
    if (!ready()) throw MessageError("MALFORMED_MESSAGE", "Handshake is not ready");
    return ack_;
}
}
