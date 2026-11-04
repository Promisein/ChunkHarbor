// Compile the real output implementation with Win32 fault wrappers only in this
// test translation unit. No runtime test switches in the shipped application.
#define NOMINMAX
#include <windows.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
namespace {
enum class Fault { None, Write, ShortWrite, Flush };
Fault fault = Fault::None;
BOOL WINAPI testWriteFile(HANDLE h, LPCVOID data, DWORD size, LPDWORD written, LPOVERLAPPED over) {
    if (fault == Fault::Write) { SetLastError(ERROR_DISK_FULL); return FALSE; }
    return WriteFile(h, data, fault == Fault::ShortWrite ? std::min<DWORD>(size, 2) : size, written, over);
}
BOOL WINAPI testFlush(HANDLE h) {
    if (fault == Fault::Flush) { SetLastError(ERROR_WRITE_FAULT); return FALSE; }
    return FlushFileBuffers(h);
}
}
#define WriteFile testWriteFile
#define FlushFileBuffers testFlush
#include "../../apps/gui/DownloadOutput.cpp"
#undef WriteFile
#undef FlushFileBuffers

namespace {
void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
void clean(const QString &root) {
    check(QDir(root).entryList({".chunkharbor-download-*"}, QDir::Files | QDir::Hidden).isEmpty(),
          "unpublished temporary must be removed");
}
template<class F> void rejects(F f) {
    bool rejected = false;
    try { f(); } catch (const std::exception &) { rejected = true; }
    check(rejected, "unsafe or failed output operation must reject");
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir temp;
        check(temp.isValid(), "private root");
        for (const auto mode : {Fault::Write, Fault::Flush, Fault::ShortWrite}) {
            const auto target = temp.filePath("恢复.bin");
            fault = mode;
            auto write = [&] {
                DownloadOutput output(target);
                output.write("hello");
                output.flush();
                output.publish();
            };
            if (mode == Fault::ShortWrite) {
                write();
                QFile result(target);
                check(result.open(QIODevice::ReadOnly) && result.readAll() == "hello", "partial writes must complete");
                result.close();
                check(QFile::remove(target), "remove test result");
            } else {
                rejects(write);
                check(!QFile::exists(target), "I/O failure must not publish");
            }
            clean(temp.path());
        }
        fault = Fault::None;
        const auto target = temp.filePath("raced.bin");
        {
            DownloadOutput output(target);
            output.write("download");
            output.flush();
            QFile raced(target);
            check(raced.open(QIODevice::WriteOnly | QIODevice::NewOnly), "create competing target");
            raced.write("keep"); raced.close();
            rejects([&] { output.publish(); });
        }
        QFile raced(target);
        check(raced.open(QIODevice::ReadOnly) && raced.readAll() == "keep", "race must not overwrite");
        clean(temp.path());
        for (const auto &name : {"../escape", "NUL", "a:stream", "COM1.txt", "trailing.", "trailing "})
            rejects([&] { DownloadOutput output(temp.path() + "/" + name); });
        rejects([&] { DownloadOutput output("//server/share/file"); });
        rejects([&] { DownloadOutput output("//?/C:/device"); });
        // A real junction needs no symbolic-link privilege on Windows.
        std::cout << "native disk-full/short-write/flush/race/path rejection and cleanup passed\n";
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
