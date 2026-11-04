#pragma once
#include <QString>
#include <memory>
#include <string_view>

// Single-owner, same-directory temporary output. No fallback that replaces a target.
class DownloadOutput final {
  public:
    explicit DownloadOutput(const QString &target);
    ~DownloadOutput();
    DownloadOutput(const DownloadOutput &) = delete;
    DownloadOutput &operator=(const DownloadOutput &) = delete;
    void write(std::string_view);
    void flush();
    void publish();
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
