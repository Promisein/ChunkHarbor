#include "chunkharbor/client_core/DirectoryScanner.h"
#include <chrono>
#include <fstream>
#include <iostream>
using namespace chunkharbor::client;
int main() {
    namespace fs = std::filesystem;
    auto root = fs::temp_directory_path() /
                ("chunkharbor-scan-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        fs::create_directories(root / "build");
        std::ofstream(root / "build" / "ignored") << "x";
        std::ofstream(root / fs::u8path("中文.txt")) << "x";
        DirectoryScanner scan(root, {"build"});
        unsigned regular = 0, excluded = 0;
        while (auto entry = scan.next()) {
            if (entry->excluded)
                ++excluded;
            else
                ++regular;
        }
        if (regular != 1 || excluded != 1)
            throw std::runtime_error("scanner totals incorrect");
        fs::remove_all(root);
        std::cout << "directory scanner passed\n";
        return 0;
    } catch (const std::exception &e) {
        fs::remove_all(root);
        std::cerr << e.what() << '\n';
        return 1;
    }
}
