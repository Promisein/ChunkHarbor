#include "chunkharbor/common/UniqueFd.h"
#include "chunkharbor/common/Logger.h"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

int main() {
    int raw = ::open("/dev/null", O_RDONLY);
    if (raw < 0) return 1;
    chunkharbor::UniqueFd first(raw);
    if (!first || first.get() != raw) return 2;
    chunkharbor::UniqueFd second(std::move(first));
    if (first || second.get() != raw) return 3;
    second.reset();
    if (::fcntl(raw, F_GETFD) != -1 || errno != EBADF) return 4;
    chunkharbor::log(chunkharbor::LogLevel::Info, "common test completed");
    return 0;
}
