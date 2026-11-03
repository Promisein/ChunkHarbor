#include "TaskModel.h"
#include "WorkspaceStore.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
static void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir temp;
        check(temp.isValid(), "temporary workspace");
        gui::FileRecord a;
        a.logicalPath = "项目/文件.txt";
        a.localPath = "C:/测试/文件.txt";
        a.size = 9007199254740993ULL;
        a.changedBlocks = 17;
        a.versionId = QString(32, 'a');
        a.sha256 = QString(64, 'b');
        a.status = "已提交";
        a.change = "新增";
        a.endpoint = "127.0.0.1:18787";
        a.strategy = gui::BackupStrategy::Resume;
        a.time = "2026-11-02T01:02:03Z";
        TaskModel tasks;
        tasks.upsert(a);
        check(tasks.rowCount() == 1 && tasks.columnCount() == 6, "task geometry includes execution strategy");
        check(tasks.data(tasks.index(0, 4)).toString() == "断点续传", "display confirmed file action");
        auto b = a;
        b.status = "失败";
        tasks.upsert(b);
        check(tasks.rowCount() == 1 && tasks.data(tasks.index(0, 0)).toString() == "失败",
              "replace task by logical path");
        {
            WorkspaceStore store(temp.path());
            check(store.records().isEmpty(), "no sample history");
            store.addCommit(a);
            store.addCommit(a);
            check(store.records().size() == 1, "idempotent commit history");
            store.appendLog("信息", "真实提交\n不能伪造新日志行");
            check(store.logs().size() == 1, "actual logs only");
        }
        {
            WorkspaceStore store(temp.path());
            check(store.records().size() == 1 && store.records()[0].size == a.size,
                  "persist exact uint64 beyond JSON float precision");
            check(store.records()[0].logicalPath == a.logicalPath, "UTF8 roundtrip");
            check(!store.logs().isEmpty(), "reload persisted logs");
        }
        QFile corrupt(temp.filePath("history.json"));
        check(corrupt.open(QIODevice::WriteOnly), "create malformed history");
        corrupt.write("{bad-json");
        corrupt.close();
        WorkspaceStore damaged(temp.path());
        check(damaged.records().isEmpty(), "bad data cannot become successful history");
        tasks.clear();
        check(tasks.rowCount() == 0, "clear model");
        std::cout << "GUI model, UTF8, exact integers, persistence and corruption passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
