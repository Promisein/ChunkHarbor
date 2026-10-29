#pragma once

#include <system_error>

namespace chunkharbor {

inline std::system_error systemError(int errorNumber, const char* operation) {
    return std::system_error(errorNumber, std::generic_category(), operation);
}

} // namespace chunkharbor
