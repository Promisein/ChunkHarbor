#pragma once
#include "chunkharbor/protocol/Download.h"
#include "chunkharbor/protocol/FilesList.h"
#include <QString>
#include <functional>

namespace gui {
// Runs in the caller's worker. Every chunk and the whole file are verified.
void receiveDownload(const QString &host, quint16 port, const chunkharbor::protocol::ListedFile &,
    chunkharbor::protocol::DownloadSelection, const std::function<void()> &check,
    const std::function<void(std::string_view)> &write, const std::function<void(quint64)> &progress);
}
