#include "DownloadOutput.h"
#include <QDir>
#include <QRegularExpression>
#include <QUuid>
#include <stdexcept>
#include <vector>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstring>
#include <cstddef>

namespace {
void nativeCheck(bool ok, const char *operation) {
    if (!ok) throw std::runtime_error(std::string(operation) + " (Windows error " +
                                     std::to_string(GetLastError()) + ")");
}
struct Handle {
    HANDLE value;
    explicit Handle(HANDLE h) : value(h) { nativeCheck(h != INVALID_HANDLE_VALUE, "open output handle failed"); }
    ~Handle() { CloseHandle(value); }
    Handle(const Handle &) = delete;
};
void component(const QString &name) {
    static const QRegularExpression reserved("^(CON|PRN|AUX|NUL|COM[1-9¹²³]|LPT[1-9¹²³])(?:\\.|$)",
                                              QRegularExpression::CaseInsensitiveOption);
    if (name.isEmpty() || name == "." || name == ".." || name.endsWith('.') || name.endsWith(' ') ||
        reserved.match(name).hasMatch()) throw std::runtime_error("invalid or reserved Windows output name");
    for (const auto ch : name)
        if (ch.unicode() < 32 || QString("<>:\"|?*").contains(ch))
            throw std::runtime_error("invalid Windows output character");
}
}
struct DownloadOutput::Impl {
    std::vector<std::unique_ptr<Handle>> directories;
    std::unique_ptr<Handle> file;
    std::wstring target;
    bool published = false;
    ~Impl() {
        if (file && !published) {
            FILE_DISPOSITION_INFO info{TRUE};
            // Delete our open file object, never a path that another process could replace.
            SetFileInformationByHandle(file->value, FileDispositionInfo, &info, sizeof(info));
        }
    }
};
DownloadOutput::DownloadOutput(const QString &path) : impl_(std::make_unique<Impl>()) {
    const auto normalized = QDir::fromNativeSeparators(path);
    if (!QRegularExpression("^[A-Za-z]:/").match(normalized).hasMatch() || normalized.size() >= 240)
        throw std::runtime_error("output requires a local absolute Windows path shorter than 240 characters");
    const auto parts = normalized.mid(3).split('/');
    for (const auto &part : parts) component(part);
    QString parent = normalized.left(3);
    auto pin = [&](const QString &directory) {
        auto handle = std::make_unique<Handle>(CreateFileW(
            QDir::toNativeSeparators(directory).toStdWString().c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        nativeCheck(GetFileInformationByHandle(handle->value, &info), "inspect output directory failed");
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("output parent is not a regular directory or is a reparse point");
        // Denying FILE_SHARE_DELETE pins every ancestor against rename/replacement.
        impl_->directories.push_back(std::move(handle));
    };
    pin(parent);
    for (qsizetype index = 0; index + 1 < parts.size(); ++index) {
        parent = QDir(parent).filePath(parts[index]);
        pin(parent);
    }
    impl_->target = QDir::toNativeSeparators(normalized).toStdWString();
    if (GetFileAttributesW(impl_->target.c_str()) != INVALID_FILE_ATTRIBUTES)
        throw std::runtime_error("target already exists; refusing overwrite");
    const auto temporary = QDir::toNativeSeparators(QDir(parent).filePath(
        ".chunkharbor-download-" + QUuid::createUuid().toString(QUuid::WithoutBraces))).toStdWString();
    impl_->file = std::make_unique<Handle>(CreateFileW(temporary.c_str(), GENERIC_WRITE | DELETE,
        0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
}
void DownloadOutput::write(std::string_view bytes) {
    while (!bytes.empty()) {
        DWORD written = 0;
        nativeCheck(WriteFile(impl_->file->value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr),
                    "disk write failed");
        if (!written) throw std::runtime_error("disk write made no progress");
        bytes.remove_prefix(written);
    }
}
void DownloadOutput::flush() {
    nativeCheck(FlushFileBuffers(impl_->file->value), "flush temporary file failed");
}
void DownloadOutput::publish() {
    const auto nameBytes = impl_->target.size() * sizeof(wchar_t);
    std::vector<unsigned char> storage(sizeof(FILE_RENAME_INFO) + nameBytes, 0);
    auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
    rename->ReplaceIfExists = FALSE;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = static_cast<DWORD>(nameBytes);
    std::memcpy(rename->FileName, impl_->target.data(), nameBytes);
    nativeCheck(SetFileInformationByHandle(impl_->file->value, FileRenameInfo, rename,
                                          static_cast<DWORD>(storage.size())),
                "publish failed; target may already exist (no overwrite)");
    impl_->published = true;
}
#else
struct DownloadOutput::Impl {};
DownloadOutput::DownloadOutput(const QString &) {
    throw std::runtime_error("GUI safe download output currently requires Windows; use Linux CLI download");
}
void DownloadOutput::write(std::string_view) { throw std::runtime_error("Windows output required"); }
void DownloadOutput::flush() { throw std::runtime_error("Windows output required"); }
void DownloadOutput::publish() { throw std::runtime_error("Windows output required"); }
#endif
DownloadOutput::~DownloadOutput() = default;
