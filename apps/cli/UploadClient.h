#pragma once
#include <cstdint>
#include <string>
void runUpload(const std::string &source, const std::string &path, const std::string &host,
               std::uint16_t port, std::uint32_t chunkSize);
