#include "chunkharbor/client_core/BackupBatch.h"
namespace chunkharbor::client {
bool BackupBatch::step(const std::function<bool()> &next) {
    if (cancelled_)
        return false;
    return next();
}
} // namespace chunkharbor::client
