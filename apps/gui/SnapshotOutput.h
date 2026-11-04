#pragma once
#include <QString>
#include <functional>
#include <memory>
#include <string_view>

namespace gui {
// A single worker owns the staging tree and its native handles.
class SnapshotOutput final {
  public:
    explicit SnapshotOutput(const QString &target);
    ~SnapshotOutput();
    void openFile(const QString &relative, quint64 size);
    void write(std::string_view);
    void flushFile();
    void verify(const std::function<void()> &check);
    void publish();
    QString cleanup() noexcept; // Empty on success, staging path if residue remains.
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
