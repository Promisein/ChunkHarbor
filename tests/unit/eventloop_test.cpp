#include "chunkharbor/common/UniqueFd.h"
#include "chunkharbor/net/Channel.h"
#include "chunkharbor/net/EventLoop.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <unistd.h>

int main() {
    int fds[2];
    if (::pipe(fds) != 0) return 1;
    chunkharbor::UniqueFd reader(fds[0]);
    chunkharbor::UniqueFd writer(fds[1]);
    chunkharbor::EventLoop loop;
    chunkharbor::Channel channel(loop, reader.get());
    std::atomic<int> tasks{0};
    int reads = 0;
    channel.setReadCallback([&] {
        char value = 0;
        if (::read(reader.get(), &value, 1) == 1 && value == 'x') ++reads;
        channel.disableAll();
        channel.remove();
        loop.quit();
    });
    channel.enableReading();
    std::thread producer([&] {
        loop.queueInLoop([&] { ++tasks; });
        const char value = 'x';
        ::write(writer.get(), &value, 1);
    });
    loop.run();
    producer.join();
    if (reads != 1 || tasks != 1) return 2;

    // A removed channel must not be dispatched when the FD becomes readable again.
    const char value = 'x';
    if (::write(writer.get(), &value, 1) != 1) return 3;
    std::thread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        loop.quit();
    });
    loop.run();
    stopper.join();
    return reads == 1 ? 0 : 4;
}
