#pragma once
#include "chunkharbor/client_core/DeltaExchange.h"
#include <filesystem>
#include <memory>
struct DeltaPlatform {
    chunkharbor::client::SourceSnapshot source;
    std::function<chunkharbor::protocol::Frame(const chunkharbor::protocol::Frame &)> exchange;
    std::shared_ptr<void> lifetime;
};
DeltaPlatform openDeltaPlatform(const std::filesystem::path &, const std::string &, std::uint16_t,
                                std::uint32_t, const std::string &);
void atomicDeltaState(const std::filesystem::path &, const std::string &);
int runDeltaCommand(int, char **);
