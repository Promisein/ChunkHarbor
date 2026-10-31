#include "chunkharbor/protocol/Handshake.h"
#include "chunkharbor/client_core/HandshakeExchange.h"
#include <iostream>
#include <stdexcept>

using namespace chunkharbor::protocol;
void check(bool value) { if (!value) throw std::runtime_error("handshake check failed"); }
template<class Fn> void rejects(Fn fn) {
    try { fn(); } catch (const MessageError&) { return; }
    throw std::runtime_error("expected MessageError");
}
template<class Fn> int nulSuffixFailures(Fn decodeBody, const std::string& body) {
    int failures = 0;
    for (const auto& suffix : {std::string(1, '\0'), std::string("\0garbage", 8), std::string(1, '\0') + body}) {
        try { decodeBody(body + suffix); ++failures; }
        catch (const MessageError& error) { check(error.code == "MALFORMED_MESSAGE"); }
    }
    rejects([&] { decodeBody(std::string(1, '\0') + body); });
    rejects([&] { decodeBody(body.substr(0, body.size()-1) + std::string(1, '\0') + "}"); });
    decodeBody(" \t\r\n" + body + " \t\r\n");
    decodeBody(body + std::string(MaxHandshakePayload-body.size(), ' '));
    rejects([&] { decodeBody(body + std::string(MaxHandshakePayload-body.size()+1, ' ')); });
    return failures;
}
int main() {
    try {
        Hello offer{"chunkharbor-cli", {1, 2}, {"handshake", "future"}};
        const auto body = encodeHello(offer);
        check(body == R"({"capabilities":["handshake","future"],"client_name":"chunkharbor-cli","versions":[1,2]})");
        const auto parsed = decodeHello(body);
        const auto ack = negotiateHello(parsed);
        check(ack.version == 1 && ack.capabilities == std::vector<std::string>{"handshake"});
        check(encodeHelloAck(ack) == R"({"capabilities":["handshake"],"max_control_payload":65536,"max_data_payload":1048576,"server_name":"chunkharbor-server","version":1})");
        check(decodeHelloAck(encodeHelloAck(ack)).maxDataPayload == MaxDataPayload);
        const auto error = decodeHandshakeError(encodeHandshakeError({"UNSUPPORTED_VERSION", "No common version"}));
        check(error.code == "UNSUPPORTED_VERSION");
        const int nulFailures = nulSuffixFailures(decodeHello, body) +
            nulSuffixFailures(decodeHelloAck, encodeHelloAck(ack)) +
            nulSuffixFailures(decodeHandshakeError, encodeHandshakeError(error));
        if (nulFailures) throw std::runtime_error("raw NUL suffixes accepted: " + std::to_string(nulFailures));
        rejects([] { decodeHelloAck(R"({"version":1,"server_name":"server\u0000","capabilities":[],"max_control_payload":65536,"max_data_payload":1048576})"); });
        rejects([] { decodeHandshakeError(R"({"code":"UNSUPPORTED_VERSION","message":"bad\u0000message"})"); });
        for (const auto* invalid : {"", "[]", "{}", "null",
             R"({"client_name":"a","versions":[1.0],"capabilities":[]})",
             R"({"client_name":"a","versions":[true],"capabilities":[]})",
             R"({"client_name":"a","versions":[-1],"capabilities":[]})",
             R"({"client_name":"a","versions":[65536],"capabilities":[]})",
             R"({"client_name":"a","versions":[18446744073709551616],"capabilities":[]})",
             R"({"client_name":"a","versions":[],"capabilities":[]})",
             R"({"client_name":"a","versions":[1,1],"capabilities":[]})",
             R"({"client_name":"a","versions":[1],"capabilities":["x","x"]})",
             R"({"client_name":"a","versions":[1],"capabilities":[],"extra":0})",
             R"({"client_name":"a","client_name":"b","versions":[1],"capabilities":[]})",
             R"({"client_name":"a","versions":[1],"capabilities":[]} garbage)",
             R"({"client_name":"a\u0000","versions":[1],"capabilities":[]})"})
            rejects([&] { decodeHello(invalid); });
        rejects([] { decodeHello(std::string(4097, ' ')); });
        rejects([] { decodeHello(std::string(1000, '[') + std::string(1000, ']')); });
        rejects([] { encodeHello({std::string(65,'a'), {1}, {}}); });
        rejects([] { encodeHello({"a", std::vector<std::uint16_t>(9, 1), {}}); });
        rejects([] { encodeHello({"a", {1}, std::vector<std::string>(17, "x")}); });
        rejects([] { negotiateHello({"a", {2}, {}}); });
        auto invalidAck = ack; invalidAck.maxControlPayload = 0;
        rejects([&] { encodeHelloAck(invalidAck); });
        rejects([] { decodeHelloAck(R"({"version":1,"server_name":"server","capabilities":[],"max_control_payload":65536.0,"max_data_payload":1048576})"); });
        rejects([] { decodeHandshakeError(R"({"code":"UNKNOWN","message":"x"})"); });
        chunkharbor::client::HandshakeExchange client(offer, 0xfedcba9876543210ULL);
        check(client.request().requestId == 0xfedcba9876543210ULL);
        client.accept({2, client.request().requestId, encodeHelloAck(ack)});
        check(client.ready());
        rejects([&] { client.accept({2, 1, encodeHelloAck(ack)}); });
        chunkharbor::client::HandshakeExchange mismatched(offer, 12);
        rejects([&] { mismatched.accept({2, 13, encodeHelloAck(ack)}); });
        auto malicious = ack; malicious.capabilities = {"not_offered"};
        chunkharbor::client::HandshakeExchange badCaps(offer, 12);
        rejects([&] { badCaps.accept({2, 12, encodeHelloAck(malicious)}); });
        chunkharbor::client::HandshakeExchange unsupported(Hello{"a", {2}, {}}, 12);
        rejects([&] { unsupported.accept({2, 12, encodeHelloAck(ack)}); });
        chunkharbor::client::HandshakeExchange failure(offer, 12);
        rejects([&] { failure.accept({15, 12, encodeHandshakeError(error)}); });
        check(!failure.ready());
        rejects([&] { failure.ack(); });
        rejects([&] { failure.accept({2, 12, encodeHelloAck(ack)}); });
        rejects([&] { chunkharbor::client::HandshakeExchange zero(offer, 0); });
        std::cout << "strict HELLO/ACK schemas, negotiation and client correlation passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
