#include "BackupSelection.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
void write(const QString &path) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    check(f.open(QIODevice::WriteOnly) && f.write("test") == 4, "fixture");
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir temp;
    try {
        const auto root = temp.path();
        write(root + "/a/main.cpp");
        write(root + "/b/main.cpp");
        write(root + "/README.md");
        write(root + "/build/ignored.cpp");
        gui::BackupConfig c;
        c.directory = root;
        c.scope = gui::BackupScope::Files;
        c.mappingRoot = root;
        c.selectedFiles = {root + "/a/main.cpp", root + "/b/main.cpp"};
        gui::RunControl control;
        auto selected = gui::selectBackupFiles(c, control);
        check(selected.entries.size() == 2 && selected.examinedFiles == 2 &&
            selected.entries[0].logicalPath != selected.entries[1].logicalPath, "selected same names");
        c.scope = gui::BackupScope::SingleFile;
        c.selectedFiles = {root + "/a/main.cpp"};
        // An unselected sibling must not be scanned or rejected by the 64 MiB limit.
        QFile large(root + "/a/unselected");
        check(large.open(QIODevice::WriteOnly) && large.resize(65LL*1024*1024), "large sibling");
        large.close();
        selected = gui::selectBackupFiles(c, control);
        check(selected.entries.size() == 1 && selected.examinedFiles == 1, "single selection expanded");
        c.selectedFiles.append(c.selectedFiles.front());
        bool rejected = false;
        try { gui::selectBackupFiles(c, control); } catch (...) { rejected = true; }
        check(rejected, "ambiguous selection accepted");
        std::cout << "selection A passed\n";
        c.scope = gui::BackupScope::Files;
        rejected = false;
        try { gui::selectBackupFiles(c, control); } catch (...) { rejected = true; }
        check(rejected, "duplicate target accepted");
        c.scope = gui::BackupScope::Rules;
        c.inclusions = {"*.cpp", "*.h"};
        c.taskExclusions = {"b/", "unselected"};
        write(root + "/中文/头文件.H");
        write(root + "/.gitignore");
        auto included = [](const gui::BackupSelection &s) {
            int count = 0;
            for (const auto &entry : s.entries) if (!entry.excluded && entry.error.empty()) ++count;
            return count;
        };
        selected = gui::selectBackupFiles(c, control);
#ifdef Q_OS_WIN
        check(included(selected) == 2, "rule suffix Unicode/case match");
#else
        check(included(selected) == 1, "case-sensitive Unix matching");
#endif
        check(selected.prunedDirectories == 2 && selected.excludedFiles == 3, "rule statistics/pruning");
        c.inclusions = {"a/main.cpp"};
        c.taskExclusions = {"*.cpp", "unselected"};
        selected = gui::selectBackupFiles(c, control);
        check(included(selected) == 0, "exclude must override include");
        c.inclusions = {"a/"};
        c.taskExclusions = {"unselected"};
        selected = gui::selectBackupFiles(c, control);
        check(included(selected) == 1, "include directory descendants");
        c.inclusions = {"./main.cpp"};
        selected = gui::selectBackupFiles(c, control);
        check(included(selected) == 0, "explicit root filename must be anchored");
        for (const auto &rule : QStringList{"../escape", "**/all", "/absolute", "a//b", "[a]"}) {
            c.inclusions = {rule};
            rejected = false;
            try { gui::selectBackupFiles(c, control); } catch (...) { rejected = true; }
            check(rejected, "invalid rule accepted");
        }
        c.inclusions.clear();
        c.taskExclusions = {"unselected"};
        c.exclusions = {"build"};
        write(root + "/a/build"); // Old bare global rules exclude directories, not files.
        // A separate parent is essential on case-insensitive NTFS: root/Build
        // would alias the existing root/build fixture rather than create Build.
        write(root + "/other/Build/legacy.cpp");
        selected = gui::selectBackupFiles(c, control);
        check(included(selected) == 7, "legacy exclusion case or dot file semantics changed");
#ifdef Q_OS_WIN
        const auto hidden = root + "/README.md";
        check(SetFileAttributesW(reinterpret_cast<LPCWSTR>(hidden.utf16()), FILE_ATTRIBUTE_HIDDEN), "hidden fixture");
        c.includeHidden = false;
        selected = gui::selectBackupFiles(c, control);
        check(included(selected) == 6, "Windows hidden attribute filtering");
        c.includeHidden = true;
        selected = gui::selectBackupFiles(c, control);
        check(included(selected) == 7, "hidden opt in");
        SetFileAttributesW(reinterpret_cast<LPCWSTR>(hidden.utf16()), FILE_ATTRIBUTE_NORMAL);
        const auto hiddenDirectory = root + "/a";
        check(SetFileAttributesW(reinterpret_cast<LPCWSTR>(hiddenDirectory.utf16()), FILE_ATTRIBUTE_HIDDEN), "hidden directory fixture");
        c.includeHidden = false;
        selected = gui::selectBackupFiles(c, control);
        check(selected.prunedDirectories == 2, "hidden directory was not pruned");
        c.includeHidden = true;
        SetFileAttributesW(reinterpret_cast<LPCWSTR>(hiddenDirectory.utf16()), FILE_ATTRIBUTE_NORMAL);
#endif
        c.scope = gui::BackupScope::SingleFile;
        c.selectedFiles = {root + "/build/ignored.cpp"};
        selected = gui::selectBackupFiles(c, control);
        check(included(selected) == 0 && selected.examinedFiles == 1, "selected file bypassed directory exclusions");
        c.selectedFiles = {root + "/a/main.cpp"};
        c.mappingRoot = root + "/a";
        selected = gui::selectBackupFiles(c, control);
        check(selected.entries.front().logicalPath == "main.cpp", "explicit root mapping");
        c.mappingRoot = root + "/b";
        rejected = false;
        try { gui::selectBackupFiles(c, control); } catch (...) { rejected = true; }
        check(rejected, "file escaped mapping root");
        c.mappingRoot = root;
        c.scope = gui::BackupScope::SingleFile;
        const auto link = root + "/source-link";
        std::error_code linkError;
        std::filesystem::create_symlink(std::filesystem::u8path((root + "/a/main.cpp").toUtf8().toStdString()),
            std::filesystem::u8path(link.toUtf8().toStdString()), linkError);
        if (!linkError) {
            c.selectedFiles = {link};
            selected = gui::selectBackupFiles(c, control);
            check(!selected.entries.front().error.empty(), "selected source followed a symbolic link");
        } else {
            std::cout << "SKIP source symlink fixture unavailable: " << linkError.message() << '\n';
        }
        write(root + "/build/deep/source.cpp");
        c.scope = gui::BackupScope::Folder;
        c.mappingRoot = root;
        c.directory = root + "/build/deep";
        selected = gui::selectBackupFiles(c, control);
        check(included(selected) == 0 && selected.prunedDirectories == 1, "nested scope bypassed excluded ancestor");
#ifdef Q_OS_WIN
        c.scope = gui::BackupScope::SingleFile;
        c.mappingRoot = root.toUpper();
        c.selectedFiles = {root + "/a/main.cpp"};
        selected = gui::selectBackupFiles(c, control);
        check(included(selected) == 1, "mapping root casing mismatch");
#endif
        std::cout << "selection B passed\n";
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
