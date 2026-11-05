#include "BackupSelection.h"
#include "chunkharbor/protocol/BeginUpload.h"
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QRegularExpression>
#include <stdexcept>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gui {
namespace {
using namespace chunkharbor::client;
namespace fs = std::filesystem;
fs::path native(const QString &path) { return fs::u8path(path.toUtf8().toStdString()); }
QString absolute(const QString &path) { return QDir::cleanPath(QFileInfo(path).absoluteFilePath()); }
bool samePath(const QString &a, const QString &b) {
#ifdef Q_OS_WIN
    return a.compare(b, Qt::CaseInsensitive) == 0;
#else
    return a == b;
#endif
}
void checkSource(const QString &source) {
    auto path = native(source);
    for (auto current = path; !current.empty();) {
        if (fs::is_symlink(fs::symlink_status(current))) throw std::runtime_error("source link rejected");
#ifdef Q_OS_WIN
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            throw std::runtime_error("source reparse point rejected");
#endif
        auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
}
QString relativeTo(const QString &root, const QString &path) {
    const auto relative = QDir(root).relativeFilePath(path);
    if (QDir::isAbsolutePath(relative) || relative == ".." || relative.startsWith("../") || relative == ".")
        throw std::runtime_error("source must be inside the local mapping root");
    return QDir::fromNativeSeparators(relative);
}
struct Rule {
    QString text;
    QRegularExpression pattern;
    bool directoryOnly, anchored;
    bool matches(const QString &relative, bool directory) const {
        if (directoryOnly && !directory) return false;
        return pattern.match(anchored ? relative : relative.section('/', -1)).hasMatch();
    }
};
QList<Rule> compileRules(const QStringList &rules, bool legacy) {
    if (rules.size() > 128) throw std::runtime_error("rule limit is 128 per group");
    QList<Rule> result;
    for (const auto &original : rules) {
        auto text = QDir::fromNativeSeparators(original.trimmed());
        // Both input separators are accepted, regardless of the host platform.
        text.replace('\\', '/');
        if (text.isEmpty() || text.size() > 1024 || text.startsWith('/') || text.contains(':') ||
            text.contains("**") || text.contains('[') || text.contains(']') || text.startsWith('!'))
            throw std::runtime_error("invalid rule: " + original.toUtf8().toStdString());
        bool dir = text.endsWith('/');
        if (dir) text.chop(1);
        const bool rooted = text.startsWith("./");
        if (text.startsWith("./")) text.remove(0, 2);
        for (const auto &part : text.split('/'))
            if (part.isEmpty() || part == "." || part == "..") throw std::runtime_error("invalid rule path");
        for (auto ch : text)
            if (ch.unicode() < 32 || ch.unicode() == 127) throw std::runtime_error("invalid rule character");
        const bool anchored = rooted || text.contains('/');
        const bool plain = !anchored && !text.contains('*') && !text.contains('?');
        QString pattern = "\\A";
        for (auto ch : text) {
            if (ch == '*') pattern += "[^/]*";
            else if (ch == '?') pattern += "[^/]";
            else pattern += QRegularExpression::escape(QString(ch));
        }
        pattern += "\\z";
        QRegularExpression::PatternOptions options = QRegularExpression::UseUnicodePropertiesOption;
#ifdef Q_OS_WIN
        // Preserve the old std::set<UTF-8 directory name> semantics exactly.
        // New task/glob/path rules follow Windows case-insensitive matching.
        if (!(legacy && plain)) options |= QRegularExpression::CaseInsensitiveOption;
#endif
        result.append({original, QRegularExpression(pattern, options), dir || (legacy && plain), anchored});
    }
    return result;
}
}
QString commonMappingRoot(const QStringList &files) {
    if (files.isEmpty()) return {};
    auto root = QFileInfo(absolute(files.front())).absolutePath();
    for (const auto &file : files) {
        const auto path = absolute(file);
        for (;;) {
            const auto relative = QDir(root).relativeFilePath(path);
            if (!QDir::isAbsolutePath(relative) && relative != ".." && !relative.startsWith("../")) break;
            const auto parent = QFileInfo(root).absolutePath();
            if (parent == root) return {}; // Different volumes require a user-defined mapping root.
            root = parent;
        }
    }
    return root;
}
BackupSelection selectBackupFiles(const BackupConfig &config, RunControl &control) {
    BackupSelection result;
    result.mappingRoot = absolute(config.mappingRoot.isEmpty() ? config.directory : config.mappingRoot);
    checkSource(result.mappingRoot);
    if (!QFileInfo(result.mappingRoot).isDir()) throw std::runtime_error("mapping root must be a directory");
    const auto includes = compileRules(config.scope == BackupScope::Rules ? config.inclusions : QStringList{}, false);
    auto excludes = compileRules(config.exclusions, true);
    excludes.append(compileRules(config.scope == BackupScope::Rules ? config.taskExclusions : QStringList{}, false));
    auto filter = [&](const fs::path &path, bool directory) -> std::string {
        if (control.checkpoint()) throw std::runtime_error("cancelled");
        const auto local = QString::fromUtf8(path.u8string());
        const auto relative = relativeTo(result.mappingRoot, local);
        checkSource(local);
        const auto state = config.stateDirectory.isEmpty() ? QString{} : absolute(config.stateDirectory);
        if (!state.isEmpty() && samePath(absolute(local), state))
            return "本地状态目录";
#ifdef Q_OS_WIN
        if (!config.includeHidden) {
            const auto attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) throw std::runtime_error("cannot read source attributes");
            if (attributes & FILE_ATTRIBUTE_HIDDEN) return "Windows 隐藏属性";
        }
#endif
        for (const auto &rule : excludes)
            if (rule.matches(relative, directory)) return ("排除规则：" + rule.text).toUtf8().toStdString();
        if (!directory && !includes.isEmpty()) {
            for (const auto &rule : includes) {
                if (rule.matches(relative, false)) return {};
                if (rule.directoryOnly) {
                    auto parent = relative;
                    while (parent.contains('/')) {
                        parent = parent.section('/', 0, -2);
                        if (rule.matches(parent, true)) return {};
                    }
                }
            }
            return "未命中包含规则";
        }
        return {};
    };
    QSet<QString> targets;
    auto append = [&](ScanEntry entry) {
        if (control.checkpoint()) throw std::runtime_error("cancelled");
        const auto local = QString::fromUtf8(entry.localPath.u8string());
        const auto relative = relativeTo(result.mappingRoot, local);
        entry.logicalPath = relative.toUtf8().toStdString();
        const auto logical = config.prefix.isEmpty() ? relative : config.prefix + '/' + relative;
        chunkharbor::protocol::encodeBeginUpload({logical.toUtf8().toStdString(), 0, 0, 1, std::string(64, '0')});
#ifdef Q_OS_WIN
        const auto key = logical.toCaseFolded();
#else
        const auto &key = logical;
#endif
        if (targets.contains(key)) throw std::runtime_error("multiple sources map to the same remote logical path");
        if (result.entries.size() >= 10000) throw std::runtime_error("backup selection exceeds 10000 entries");
        targets.insert(key);
        if (entry.excluded && QFileInfo(local).isDir()) ++result.prunedDirectories;
        else {
            ++result.examinedFiles;
            if (entry.excluded) ++result.excludedFiles;
        }
        result.entries.push_back(std::move(entry));
    };
    if (config.scope == BackupScope::SingleFile || config.scope == BackupScope::Files) {
        if (config.selectedFiles.isEmpty() || config.selectedFiles.size() > 10000 ||
            (config.scope == BackupScope::SingleFile && config.selectedFiles.size() != 1))
            throw std::runtime_error("invalid explicit file selection");
        for (const auto &file : config.selectedFiles) {
            const auto local = absolute(file);
            relativeTo(result.mappingRoot, local);
            ScanEntry entry{native(local), {}, false, {}, {}};
            try {
                checkSource(local);
                if (!QFileInfo(local).isFile()) throw std::runtime_error("selected source is missing or not a regular file");
                // Inspect only the selected file's ancestors, never enumerate siblings.
                auto ancestor = QFileInfo(local).absolutePath();
                while (!samePath(ancestor, result.mappingRoot)) {
                    entry.exclusionReason = filter(native(ancestor), true);
                    if (!entry.exclusionReason.empty()) break;
                    const auto parent = QFileInfo(ancestor).absolutePath();
                    if (parent == ancestor) throw std::runtime_error("source mapping boundary changed");
                    ancestor = parent;
                }
                if (entry.exclusionReason.empty()) entry.exclusionReason = filter(native(local), false);
                entry.excluded = !entry.exclusionReason.empty();
                if (!entry.excluded && QFileInfo(local).size() > 64LL*1024*1024)
                    throw std::runtime_error("selected source exceeds 64 MiB");
            } catch (const std::exception &error) { entry.error = error.what(); }
            append(std::move(entry));
        }
    } else {
        const auto scanRoot = absolute(config.directory);
        checkSource(scanRoot);
        if (!samePath(scanRoot, result.mappingRoot)) {
            relativeTo(result.mappingRoot, scanRoot);
            auto ancestor = scanRoot;
            while (!samePath(ancestor, result.mappingRoot)) {
                const auto reason = filter(native(ancestor), true);
                if (!reason.empty()) {
                    append({native(scanRoot), {}, true, {}, reason});
                    return result;
                }
                const auto parent = QFileInfo(ancestor).absolutePath();
                if (samePath(parent, ancestor)) throw std::runtime_error("source mapping boundary changed");
                ancestor = parent;
            }
        }
        DirectoryScanner scanner(native(scanRoot), {}, native(config.stateDirectory), filter);
        while (auto entry = scanner.next()) append(std::move(*entry));
    }
    return result;
}
}
