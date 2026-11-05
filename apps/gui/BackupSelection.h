#pragma once
#include "TransferTypes.h"
#include "chunkharbor/client_core/DirectoryScanner.h"
namespace gui {
struct BackupSelection {
    std::vector<chunkharbor::client::ScanEntry> entries;
    QString mappingRoot;
    int examinedFiles = 0, excludedFiles = 0, prunedDirectories = 0;
};
BackupSelection selectBackupFiles(const BackupConfig &, RunControl &);
QString commonMappingRoot(const QStringList &files);
}
