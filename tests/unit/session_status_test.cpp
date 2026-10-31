#include "chunkharbor/protocol/SessionStatus.h"
#include <stdexcept>
using namespace chunkharbor::protocol;
void check(bool ok) { if (!ok) throw std::runtime_error("SESSION_STATUS test failed"); }
template <class F> void rejects(F f) {
    try { f(); } catch (const UploadPlanError &) { return; }
    throw std::runtime_error("expected SESSION_STATUS rejection");
}
int main() {
    SessionStatus status;
    status.chunkSize = 1;
    status.blockCount = 65536;
    status.missing.assign(8192, 0x55); // 32768 alternating missing ranges.
    auto wire = encodeSessionStatus(status);
    check(wire.size() < MaxControlPayload);
    check(decodeSessionStatus(wire).missing == status.missing);
    validateSessionStatusForFile(status, 65536, 1);
    rejects([&] { validateSessionStatusForFile(status, 65535, 1); });
    rejects([&] { decodeSessionStatus(wire + std::string(1, '\0')); });
    rejects([&] { decodeSessionStatus("{}"); });
    rejects([&] { decodeSessionStatus(wire.substr(0, wire.size() - 1) + ",\"extra\":0}"); });
    status.blockCount = 1;
    status.missing = {0x02};
    rejects([&] { encodeSessionStatus(status); });
}
