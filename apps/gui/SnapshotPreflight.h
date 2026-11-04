#pragma once
#include "chunkharbor/protocol/FilesList.h"
#include <QString>
#include <atomic>

namespace gui {
struct SnapshotPathCheck {
    QString pathError, conflict;
};
struct SnapshotPreflightResult {
    QString rootError;
    std::vector<SnapshotPathCheck> rows;
};
// Read-only advisory inspection. The executor must inspect and pin the paths again.
SnapshotPreflightResult preflightSnapshot(const QString &directory,
    const std::vector<chunkharbor::protocol::ListedFile> &, const std::atomic_bool &cancelled,
    bool newDirectory = false);
bool snapshotSafeComponent(const QString &name);
QString snapshotTargetPath(const QString &directory, const std::string &relative);
}
