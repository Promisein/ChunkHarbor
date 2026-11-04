#include "SnapshotPreflight.h"
#include <QCoreApplication>
#include <QDir>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gui {
namespace {
QString label(const char *message) { return QCoreApplication::translate("SnapshotPreflight", message); }
QString normalizedRoot(const QString &directory) {
    auto root = QDir::fromNativeSeparators(directory);
    while (root.size() > 3 && root.endsWith('/')) root.chop(1);
    return root;
}
bool safeComponent(const QString &name) {
    static const QRegularExpression reserved(
        R"(^(CON|PRN|AUX|NUL|CONIN[$]|CONOUT[$]|COM[1-9¹²³]|LPT[1-9¹²³]) *(?:\.|$))",
        QRegularExpression::CaseInsensitiveOption);
    if (name.isEmpty() || name == "." || name == ".." || name.size() > 255 ||
        name.endsWith('.') || name.endsWith(' ') || reserved.match(name).hasMatch()) return false;
    for (const auto ch : name)
        if (ch.unicode() < 32 || ch.unicode() == 127 || QString("<>:\"/\\|?*").contains(ch)) return false;
    return true;
}
bool safeParts(const QStringList &parts) {
    return std::all_of(parts.begin(), parts.end(), safeComponent);
}
QString join(const QString &parent, const QString &name) {
    return parent.endsWith('/') ? parent + name : parent + '/' + name;
}
void addConflict(SnapshotPathCheck &check, const QString &text) {
    if (check.conflict.isEmpty()) check.conflict = text;
    else if (!check.conflict.contains(text)) check.conflict += " / " + text;
}

#ifdef Q_OS_WIN
// Probe one component at a time. Never deliberately walk through a known reparse point.
QString inspect(QString parent, const QStringList &parts, bool root, const std::atomic_bool &cancelled) {
    for (qsizetype i = 0; i < parts.size(); ++i) {
        if (cancelled) return {};
        parent = join(parent, parts[i]);
        const auto path = QDir::toNativeSeparators(parent).toStdWString();
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                return root ? label("目标目录不存在") : QString{};
            return label("无法读取目标属性（Windows %1）").arg(error);
        }
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return label("目标路径含重解析点或链接");
        if (!root && i + 1 == parts.size()) return label("目标已存在");
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return label("父路径被文件占用");
    }
    return {};
}
#endif
}

bool snapshotSafeComponent(const QString &name) { return safeComponent(name); }

QString snapshotTargetPath(const QString &directory, const std::string &relative) {
    return directory.isEmpty() ? QString{} : join(normalizedRoot(directory), QString::fromUtf8(relative));
}

SnapshotPreflightResult preflightSnapshot(const QString &directory,
    const std::vector<chunkharbor::protocol::ListedFile> &files, const std::atomic_bool &cancelled, bool newDirectory) {
    SnapshotPreflightResult result;
    result.rows.resize(files.size());
    const auto root = normalizedRoot(directory);
    static const QRegularExpression localDrive("^[A-Za-z]:/");
    if (!root.isEmpty() && (root.size() >= 240 || (newDirectory && root.size() <= 3) || !localDrive.match(root).hasMatch() ||
        (root.size() > 3 && !safeParts(root.mid(3).split('/')))))
        result.rootError = label("请选择有效的 Windows 本地盘目录，路径须短于 240 个 UTF-16 单元");
#ifdef Q_OS_WIN
    if (!root.isEmpty() && result.rootError.isEmpty()) {
        const auto drive = QDir::toNativeSeparators(root.left(3)).toStdWString();
        const auto kind = GetDriveTypeW(drive.c_str());
        if (newDirectory && kind != DRIVE_FIXED)
            result.rootError = label("恢复要求本机固定 NTFS 盘");
        else if (kind != DRIVE_FIXED && kind != DRIVE_REMOVABLE && kind != DRIVE_RAMDISK)
            result.rootError = label("目标必须位于可访问的本地盘");
        else {
            const auto attributes = GetFileAttributesW(drive.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
                result.rootError = label("无法确认目标盘根目录安全可用");
            else if (root.size() > 3) {
                auto parts = root.mid(3).split('/');
                if (newDirectory) parts.removeLast();
                result.rootError = inspect(root.left(3), parts, true, cancelled);
                if (newDirectory && result.rootError.isEmpty() && !cancelled) {
                    const auto path = QDir::toNativeSeparators(root).toStdWString();
                    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
                        result.rootError = label("恢复目标已存在");
                    else if (GetLastError() != ERROR_FILE_NOT_FOUND)
                        result.rootError = label("无法确认恢复目标为空");
                }
            }
        }
    }
#else
    if (!root.isEmpty() && result.rootError.isEmpty()) result.rootError = label("Windows 文件系统预检须在 Windows 上运行");
#endif

    std::vector<std::pair<QString, std::size_t>> keys;
    keys.reserve(files.size());
    for (std::size_t i = 0; i < files.size() && !cancelled; ++i) {
        const auto relative = QString::fromUtf8(files[i].path);
        const auto parts = relative.split('/');
        auto &check = result.rows[i];
        if (!safeParts(parts)) {
            check.pathError = label("非法相对路径、保留名或非法字符");
            continue;
        }
        if (join(root, relative).size() >= 240) {
            check.pathError = label("本地目标路径过长");
            continue;
        }
        keys.emplace_back(relative.toCaseFolded(), i);
#ifdef Q_OS_WIN
        if (!newDirectory && !root.isEmpty() && result.rootError.isEmpty())
            check.conflict = inspect(root, parts, false, cancelled);
#endif
    }
    if (cancelled) return result;
    std::sort(keys.begin(), keys.end());
    QSet<QString> aliases;
    for (std::size_t j = 1; j < keys.size() && !cancelled; ++j) {
        const auto &[previous, before] = keys[j - 1];
        const auto &[key, index] = keys[j];
        if (previous == key) {
            const auto issue = label("大小写不敏感目标碰撞");
            addConflict(result.rows[before], issue);
            addConflict(result.rows[index], issue);
        }
        const auto a = QString::fromUtf8(files[before].path).split('/');
        const auto b = QString::fromUtf8(files[index].path).split('/');
        QString prefix;
        for (qsizetype part = 0; part + 1 < std::min(a.size(), b.size()); ++part) {
            const auto folded = a[part].toCaseFolded();
            if (folded != b[part].toCaseFolded()) break;
            prefix = prefix.isEmpty() ? folded : prefix + '/' + folded;
            if (a[part] != b[part]) { aliases.insert(prefix); break; }
        }
    }
    for (const auto &[key, index] : keys) {
        if (cancelled) break;
        for (auto slash = key.indexOf('/'); slash >= 0; slash = key.indexOf('/', slash + 1)) {
            const auto parent = key.left(slash);
            const auto file = std::lower_bound(keys.begin(), keys.end(), parent,
                [](const auto &item, const QString &name) { return item.first < name; });
            if (file != keys.end() && file->first == parent) {
                const auto issue = label("文件与目录路径冲突");
                addConflict(result.rows[file->second], issue);
                addConflict(result.rows[index], issue);
            }
            if (aliases.contains(parent)) addConflict(result.rows[index], label("目录大小写拼写冲突"));
        }
    }
    return result;
}
}
