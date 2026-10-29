#include "chunkharbor/common/Logger.h"

#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "Usage: chunkharbor-cli --help\n"
                  << "M1 event loop foundation; file transfer is not implemented yet.\n";
        return 0;
    }
    chunkharbor::log(chunkharbor::LogLevel::Error,
                     "client operation is not implemented; run --help");
    return 2;
}
