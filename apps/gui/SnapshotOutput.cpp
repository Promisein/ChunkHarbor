#include "SnapshotOutput.h"
#include "SnapshotPreflight.h"
#include <QDir>
#include <QStringList>
#include <QUuid>
#include <stdexcept>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#include <sddl.h>
#include <cstddef>
#include <cstring>
#include <map>
#include <vector>

namespace {
using namespace gui;
struct CloseHandleDeleter { void operator()(void *handle) const { CloseHandle(handle); } };
using Handle = std::unique_ptr<void, CloseHandleDeleter>;

[[noreturn]] void windowsError(const char *operation, DWORD error = GetLastError()) {
    throw std::runtime_error(std::string(operation) + " (Windows " + std::to_string(error) + ")");
}
BY_HANDLE_FILE_INFORMATION fileInfo(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) windowsError("inspect restore object");
    return info;
}
quint64 fileId(const BY_HANDLE_FILE_INFORMATION &info) {
    return (quint64{info.nFileIndexHigh} << 32) | info.nFileIndexLow;
}
bool regular(DWORD attributes, bool directory) {
    return !(attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        bool(attributes & FILE_ATTRIBUTE_DIRECTORY) == directory;
}
Handle relativeOpen(HANDLE parent, const QString &name, ACCESS_MASK access, ULONG share,
    ULONG disposition, ULONG options, PSECURITY_DESCRIPTOR security = nullptr, bool missingOK = false) {
    auto spelling = name.toStdWString();
    const auto bytes = static_cast<USHORT>(spelling.size() * sizeof(wchar_t));
    UNICODE_STRING component{bytes, bytes, spelling.data()};
    OBJECT_ATTRIBUTES attributes{};
    InitializeObjectAttributes(&attributes, &component, OBJ_CASE_INSENSITIVE | OBJ_DONT_REPARSE, parent, security);
    IO_STATUS_BLOCK result{};
    HANDLE handle = nullptr;
    const auto status = NtCreateFile(&handle, access | SYNCHRONIZE, &attributes, &result, nullptr,
        FILE_ATTRIBUTE_NORMAL, share, disposition,
        options | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
    if (status < 0) {
        const auto error = RtlNtStatusToDosError(status);
        if (missingOK && error == ERROR_FILE_NOT_FOUND) return {};
        windowsError(disposition == FILE_CREATE ? "create new restore object (name conflict or I/O error)"
                                               : "open restore object", error);
    }
    return Handle(handle);
}
bool erase(HANDLE handle) noexcept {
    FILE_DISPOSITION_INFO disposition{TRUE};
    return SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition));
}
constexpr ACCESS_MASK DirectoryRead = FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES;
constexpr ULONG SharePinned = FILE_SHARE_READ | FILE_SHARE_WRITE;

struct Node {
    std::map<QString, Node> children;
    Handle file;
    quint64 id = 0, size = 0;
    bool owned = false, directory = false;
};
Handle openOwned(HANDLE parent, const QString &name, const Node &node) {
    auto handle = relativeOpen(parent, name, DELETE | FILE_READ_ATTRIBUTES | (node.directory ? DirectoryRead : 0),
        FILE_SHARE_READ, FILE_OPEN, node.directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE);
    const auto info = fileInfo(handle.get());
    if (!regular(info.dwFileAttributes, node.directory) || fileId(info) != node.id ||
        (!node.directory && info.nNumberOfLinks != 1))
        throw std::runtime_error("restore staging object changed identity");
    return handle;
}

bool removeChildren(HANDLE directory, Node &node) noexcept {
    bool removed = true;
    for (auto &[name, child] : node.children) {
        if (!child.owned) continue;
        try {
            auto handle = child.file ? std::move(child.file) : openOwned(directory, name, child);
            if (child.directory && !removeChildren(handle.get(), child)) removed = false;
            if (!erase(handle.get())) removed = false;
        } catch (...) { removed = false; }
    }
    return removed;
}

using DirectoryBuffer = unsigned char[8192];
void verifyTree(HANDLE directory, const Node &node, const std::function<void()> &check, DirectoryBuffer &buffer) {
    std::size_t count = 0;
    auto mode = FileIdBothDirectoryRestartInfo;
    for (;;) {
        check();
        if (!GetFileInformationByHandleEx(directory, mode, buffer, sizeof(buffer))) {
            const auto error = GetLastError();
            if (error == ERROR_NO_MORE_FILES) break;
            windowsError("enumerate staging directory", error);
        }
        mode = FileIdBothDirectoryInfo;
        for (auto *entry = reinterpret_cast<FILE_ID_BOTH_DIR_INFO *>(buffer);;) {
            const auto name = QString::fromWCharArray(entry->FileName, entry->FileNameLength / sizeof(wchar_t));
            if (name != "." && name != "..") {
                const auto child = node.children.find(name);
                if (child == node.children.end() || !child->second.owned ||
                    static_cast<quint64>(entry->FileId.QuadPart) != child->second.id ||
                    !regular(entry->FileAttributes, child->second.directory) ||
                    (!child->second.directory && static_cast<quint64>(entry->EndOfFile.QuadPart) != child->second.size))
                    throw std::runtime_error("unexpected or modified staging entry");
                ++count;
            }
            if (!entry->NextEntryOffset) break;
            entry = reinterpret_cast<FILE_ID_BOTH_DIR_INFO *>(
                reinterpret_cast<unsigned char *>(entry) + entry->NextEntryOffset);
        }
    }
    if (count != node.children.size()) throw std::runtime_error("incomplete restore staging tree");
    for (const auto &[name, child] : node.children) {
        check();
        if (child.directory) {
            auto handle = openOwned(directory, name, child);
            verifyTree(handle.get(), child, check, buffer);
        } else if (fileInfo(child.file.get()).nNumberOfLinks != 1) {
            throw std::runtime_error("staging file has an unexpected hard link");
        }
    }
}

void closeFiles(Node &node) {
    node.file.reset();
    for (auto &[name, child] : node.children) closeFiles(child);
}
}
#endif

namespace gui {
#ifdef Q_OS_WIN
struct SnapshotOutput::Impl {
    std::vector<Handle> parents;
    Handle staging;
    Node root;
    QString stagingPath, finalName;
    HANDLE current = nullptr;
};
SnapshotOutput::SnapshotOutput(const QString &target) : impl_(std::make_unique<Impl>()) {
    const auto path = QDir::fromNativeSeparators(target);
    if (path.size() <= 3 || path.size() >= 240 || path[1] != ':' || path[2] != '/')
        throw std::runtime_error("invalid absolute restore target");
    auto parts = path.mid(3).split('/');
    for (const auto &part : parts)
        if (!snapshotSafeComponent(part)) throw std::runtime_error("invalid Windows restore target component");
    impl_->finalName = parts.takeLast();
    if (impl_->finalName.startsWith(".chunkharbor-restore-", Qt::CaseInsensitive))
        throw std::runtime_error("restore target uses reserved staging prefix");
    const auto drive = QDir::toNativeSeparators(path.left(3)).toStdWString();
    if (GetDriveTypeW(drive.c_str()) != DRIVE_FIXED) throw std::runtime_error("restore requires a fixed local NTFS drive");
    Handle root(CreateFileW(drive.c_str(), DirectoryRead, SharePinned, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (root.get() == INVALID_HANDLE_VALUE) {
        root.release();
        windowsError("open restore drive");
    }
    wchar_t fs[16]{};
    if (!regular(fileInfo(root.get()).dwFileAttributes, true) ||
        !GetVolumeInformationByHandleW(root.get(), nullptr, 0, nullptr, nullptr, nullptr, fs, 16) ||
        std::wstring_view(fs) != L"NTFS") throw std::runtime_error("restore requires a fixed local NTFS drive");
    impl_->parents.push_back(std::move(root));
    for (const auto &part : parts) {
        auto dir = relativeOpen(impl_->parents.back().get(), part, DirectoryRead, SharePinned,
            FILE_OPEN, FILE_DIRECTORY_FILE);
        if (!regular(fileInfo(dir.get()).dwFileAttributes, true))
            throw std::runtime_error("restore parent contains a reparse point");
        impl_->parents.push_back(std::move(dir));
    }
    const auto parent = impl_->parents.back().get();
    const auto slash = path.lastIndexOf('/');
    const auto parentPath = slash == 2 ? path.left(3) : path.left(slash);
    wchar_t actual[512]{};
    const auto length = GetFinalPathNameByHandleW(parent, actual, 512, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!length || length >= 512 ||
        QDir::fromNativeSeparators(QString::fromWCharArray(actual + 4)).compare(parentPath, Qt::CaseInsensitive))
        throw std::runtime_error("restore parent uses a short name or another path alias");
    if (relativeOpen(parent, impl_->finalName, FILE_READ_ATTRIBUTES, SharePinned | FILE_SHARE_DELETE,
        FILE_OPEN, 0, nullptr, true)) throw std::runtime_error("restore target already exists");

    const auto temporary = ".chunkharbor-restore-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    impl_->stagingPath = QDir(parentPath).filePath(temporary);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)", SDDL_REVISION_1, &descriptor, nullptr))
        windowsError("create staging security descriptor");
    const std::unique_ptr<void, decltype(&LocalFree)> security(descriptor, LocalFree);
    impl_->staging = relativeOpen(parent, temporary, DirectoryRead | DELETE, FILE_SHARE_READ,
        FILE_CREATE, FILE_DIRECTORY_FILE, descriptor);
}
SnapshotOutput::~SnapshotOutput() { cleanup(); }
void SnapshotOutput::openFile(const QString &relative, quint64 size) {
    const auto parts = relative.split('/');
    for (const auto &part : parts)
        if (!snapshotSafeComponent(part)) throw std::runtime_error("invalid restore entry path");
    auto *node = &impl_->root;
    HANDLE parent = impl_->staging.get();
    std::vector<Handle> ancestors;
    for (qsizetype index = 0; index + 1 < parts.size(); ++index) {
        const auto &name = parts[index];
        auto [it, inserted] = node->children.try_emplace(name);
        node = &it->second;
        if (inserted) {
            auto created = relativeOpen(parent, name, DirectoryRead | DELETE, FILE_SHARE_READ,
                FILE_CREATE, FILE_DIRECTORY_FILE);
            try { node->id = fileId(fileInfo(created.get())); }
            catch (...) { erase(created.get()); throw; }
            node->directory = node->owned = true;
            ancestors.push_back(std::move(created));
        } else {
            if (!node->directory) throw std::runtime_error("file and directory restore path conflict");
            ancestors.push_back(openOwned(parent, name, *node));
        }
        parent = ancestors.back().get();
    }
    auto [it, inserted] = node->children.try_emplace(parts.back());
    if (!inserted) throw std::runtime_error("duplicate restore path");
    auto &file = it->second;
    file.file = relativeOpen(parent, parts.back(), GENERIC_WRITE | FILE_READ_ATTRIBUTES | DELETE, 0,
        FILE_CREATE, FILE_NON_DIRECTORY_FILE);
    file.owned = true;
    file.id = fileId(fileInfo(file.file.get()));
    file.size = size;
    impl_->current = file.file.get();
}
void SnapshotOutput::write(std::string_view bytes) {
    while (!bytes.empty()) {
        DWORD written = 0;
        if (!WriteFile(impl_->current, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr))
            windowsError("write staged file");
        if (!written) throw std::runtime_error("staging write made no progress");
        bytes.remove_prefix(written);
    }
}
void SnapshotOutput::flushFile() {
    if (!FlushFileBuffers(impl_->current)) windowsError("flush staged file");
    impl_->current = nullptr;
}
void SnapshotOutput::verify(const std::function<void()> &check) {
    if (!regular(fileInfo(impl_->staging.get()).dwFileAttributes, true))
        throw std::runtime_error("restore staging root became a reparse point");
    alignas(FILE_ID_BOTH_DIR_INFO) DirectoryBuffer buffer;
    verifyTree(impl_->staging.get(), impl_->root, check, buffer);
}
void SnapshotOutput::publish() {
    const auto name = impl_->finalName.toStdWString();
    const auto bytes = name.size() * sizeof(wchar_t);
    const auto length = sizeof(FILE_RENAME_INFORMATION) + bytes;
    std::vector<quint64> storage((length + sizeof(quint64) - 1) / sizeof(quint64), 0);
    auto *rename = reinterpret_cast<FILE_RENAME_INFORMATION *>(storage.data());
    rename->ReplaceIfExists = FALSE;
    rename->RootDirectory = impl_->parents.back().get();
    rename->FileNameLength = static_cast<DWORD>(bytes);
    std::memcpy(rename->FileName, name.data(), bytes);
    // NTFS refuses directory rename with open child files. Close only at the commit boundary.
    closeFiles(impl_->root);
    IO_STATUS_BLOCK result{};
    const auto status = NtSetInformationFile(impl_->staging.get(), &result, rename,
        static_cast<ULONG>(length), FileRenameInformation);
    if (status < 0) windowsError("publish restore directory (no overwrite)", RtlNtStatusToDosError(status));
    impl_->staging.reset();
}
QString SnapshotOutput::cleanup() noexcept {
    if (!impl_->staging) return {};
    const auto removed = removeChildren(impl_->staging.get(), impl_->root);
    const auto removedRoot = erase(impl_->staging.get());
    impl_->staging.reset();
    return removed && removedRoot ? QString{} : impl_->stagingPath;
}
#else
struct SnapshotOutput::Impl {};
SnapshotOutput::SnapshotOutput(const QString &) { throw std::runtime_error("restore requires Windows NTFS"); }
SnapshotOutput::~SnapshotOutput() = default;
void SnapshotOutput::openFile(const QString &, quint64) {}
void SnapshotOutput::write(std::string_view) {}
void SnapshotOutput::flushFile() {}
void SnapshotOutput::verify(const std::function<void()> &) {}
void SnapshotOutput::publish() {}
QString SnapshotOutput::cleanup() noexcept { return {}; }
#endif
}
