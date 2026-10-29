cmake_minimum_required(VERSION 3.16)
project(ChunkHarbor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(chunkharbor_core STATIC
    src/common/Logger.cpp
    src/net/EventLoop.cpp
    src/net/EpollPoller.cpp
    src/net/Channel.cpp
)
target_include_directories(chunkharbor_core PUBLIC include)
target_compile_options(chunkharbor_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(chunkharbor-server apps/server/main.cpp)
target_link_libraries(chunkharbor-server PRIVATE chunkharbor_core)

add_executable(chunkharbor-cli apps/cli/main.cpp)
target_link_libraries(chunkharbor-cli PRIVATE chunkharbor_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(chunkharbor-common-test tests/unit/common_test.cpp)
    target_link_libraries(chunkharbor-common-test PRIVATE chunkharbor_core)
    add_test(NAME common_test COMMAND chunkharbor-common-test)
    add_test(NAME server_help COMMAND chunkharbor-server --help)
    add_test(NAME cli_help COMMAND chunkharbor-cli --help)
    add_executable(chunkharbor-eventloop-test tests/unit/eventloop_test.cpp)
    target_link_libraries(chunkharbor-eventloop-test PRIVATE chunkharbor_core)
    add_test(NAME eventloop_test COMMAND chunkharbor-eventloop-test)
    set_tests_properties(eventloop_test PROPERTIES TIMEOUT 5)
endif()
