#pragma once

#include "chunkharbor/common/UniqueFd.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace chunkharbor {

class Channel;

class EpollPoller {
public:
    struct ActiveEvent { std::uint64_t token; std::uint32_t events; };
    EpollPoller();
    void update(Channel& channel);
    void remove(Channel& channel);
    std::vector<ActiveEvent> poll();
    Channel* find(std::uint64_t token) const noexcept;

private:
    struct Entry { int fd; Channel* channel; };
    UniqueFd epollFd_;
    std::uint64_t nextToken_ = 1;
    std::unordered_map<int, std::uint64_t> tokensByFd_;
    std::unordered_map<std::uint64_t, Entry> entries_;
};

} // namespace chunkharbor
