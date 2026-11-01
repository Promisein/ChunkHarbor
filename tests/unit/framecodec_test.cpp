#include "chunkharbor/client_core/EchoExchange.h"
#include "chunkharbor/protocol/FrameCodec.h"
#include <iostream>
#include <stdexcept>

using namespace chunkharbor::protocol;
void check(bool value) {
    if (!value)
        throw std::runtime_error("frame check failed");
}
int main() {
    try {
        Frame f{16, 0x0102030405060708ULL, "abc"};
        const unsigned char golden[] = {0x43, 0x48, 0x4b, 0x48, 0, 1, 0, 16, 0,   0,   0,  0,
                                        1,    2,    3,    4,    5, 6, 7, 8,  0,   0,   0,  0,
                                        0,    0,    0,    3,    0, 0, 0, 0,  'a', 'b', 'c'};
        const std::string expected(reinterpret_cast<const char *>(golden), sizeof(golden));
        const auto bytes = encode(f);
        check(bytes == expected);
        for (std::size_t i = 0; i < bytes.size(); ++i)
            check(decode(std::string_view(bytes).substr(0, i)).status == DecodeStatus::NeedMore);
        auto result = decode(bytes + bytes);
        check(result.status == DecodeStatus::Complete && result.consumed == bytes.size());
        check(result.frame.type == f.type && result.frame.requestId == f.requestId &&
              result.frame.payload == "abc");
        for (int pos : {0, 5, 7, 8, 31}) {
            auto bad = bytes;
            bad[pos] = static_cast<char>(255);
            check(decode(bad).status == DecodeStatus::Error);
        }
        auto huge = bytes.substr(0, 32);
        for (int i = 20; i < 28; ++i)
            huge[i] = static_cast<char>(255);
        check(decode(huge).status == DecodeStatus::Error);
        check(decode(encode(Frame{16, 0, {}})).status == DecodeStatus::Complete);
        check(decode(encode(Frame{5, 99, std::string(1024 * 1024, '\0')})).frame.payload.size() ==
              1024 * 1024);
        bool rejected = false;
        try {
            encode(Frame{16, 0, std::string(65537, 'x')});
        } catch (const std::length_error &) {
            rejected = true;
        }
        check(rejected);
        for (std::uint16_t type = 24; type <= 31; ++type)
            check(decode(encode(Frame{type, 1, "{}"})).frame.type == type);
        rejected = false;
        try {
            encode(Frame{32, 1, ""});
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        check(rejected);
        auto ping = decode(chunkharbor::client::echoRequest("portable"));
        check(ping.frame.type == 16 && ping.frame.requestId == 1);
        ping.frame.type = 17;
        check(chunkharbor::client::isEchoResponse(ping.frame, "portable"));
        check(!chunkharbor::client::isEchoResponse(ping.frame, "mismatch"));
        std::cout << "golden vector, every split and malformed frames passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
