# R2.2 服务端历史版本查询

版本页选择“查看服务器历史”，输入逻辑路径后点击“查询最新”。若本机记录中已经选中一个文件，页面会填入该路径。查询使用当前连接设置，支持查看更早版本与取消。本机提交记录保留原有入口，服务端历史页只展示查询结果。

## 版本和迁移

- schema v2 的 `versions(id,path,size,chunk_size,sha256)` 保存不可变版本元数据，`version_chunks(version_id,idx)` 保存该版本的块摘要，`current_files(path,version_id)` 指向已发布的当前版本。旧上传提交也可能暂存在 `transfers`，由既有流程整理为 `versions`。v2 没有版本提交时间。
- schema v3 增加 `version_history(sequence,version_id,path,committed_at_s)` 和发布触发器。增量上传在更新当前指针的事务中登记，旧上传在状态首次变为 COMMITTED 时登记。复用同一版本 ID 保留原有序号和时间。显式存在性条件用于避开外层 UPSERT 对触发器冲突策略的影响。
- 迁移在 `BEGIN IMMEDIATE` 到 `COMMIT` 内回填历史、建立索引和触发器，并一起推进两个 schema 版本标记。旧版记录的时间为 NULL。旧版本按同一路径内的 v2 登记 rowid 顺序赋予持久序号，旧上传作为该路径的起始版本。增量写入在发布前会先登记对象，回填排除只有 phase 4 引用且没有发布证据的对象。启动修复若完成该对象的首次数据库发布，记录的是实际完成发布事务时的 UTC 秒。
- 已提交事务中的序号单调增加，允许间隙。时间来自服务端墙钟，可能重复或回拨，因此只用于展示。迁移只加历史索引数据，原有版本 ID、块、当前指针和会话字段不变。v3 数据库使用 v3 服务端，降回旧二进制需恢复升级前的备份副本。

## 协议与分页

HELLO 在客户端主动声明后协商 `version_history_v1`。请求帧 34 使用 `{path,before,limit}`，默认客户端页大小 32，服务端接受 1–64。首屏的 `before` 为 null，后续传上页返回的独占序号。响应帧 35 使用 `{path,versions,next_before}`，其中每项包含 `version_id`、`size`、`sha256`、`sequence`、`committed_at_s`、`current`。末页和空页的 `next_before` 为 null。沿用协议控制帧的 64 KiB 上限，路径最多 4096 UTF-8 字节。

SQLite 使用 `(path,sequence DESC)` 复合索引按路径和游标定位，最多读取 limit + 1 行。多读一行用于判断是否还有下一页。翻页期间的新版本获得更大的序号，后续页的游标会排除它们。刷新首屏可看到新增项。当前标记按每次查询的数据库状态计算，分页浏览期间可能变化。实测查询计划显示 `SEARCH h USING INDEX history_by_path_sequence (path=? AND sequence<?)`，其余三张表以主键或唯一索引关联，无临时排序 B 树。参见 [SQLite EXPLAIN QUERY PLAN](https://www.sqlite.org/eqp.html) 和 [SQLite AUTOINCREMENT](https://www.sqlite.org/autoinc.html)。

服务端在有界磁盘工作池内读取 SQLite，网络线程负责收发。Qt 页面用异步请求，每次查询持有独立的 Transport。换路径、换端点、取消、切回本机记录或销毁页面时停掉旧连接。返回内容要通过请求 ID、路径、行数和序号边界核验。旧客户端仍可协商原有能力，`download_v1` 的版本 ID 仍是当前版本匹配条件。

## 变更文件

| 范围 | 文件 |
| --- | --- |
| 协议 | `VersionHistory.h/.cpp`、`FrameCodec.cpp` |
| 服务端 | `schema_v3.sql`、`SqliteStore.h/.cpp`、`UploadStorage.h/.cpp`、`UploadHandler.cpp` |
| GUI | `VersionsPage.h/.cpp`、`MainWindow.cpp` |
| 验证 | `CMakeLists.txt`、`framecodec_test.cpp`、`server_history_gui_test.cpp`、`version_history_test.py`、`windows_gui_history_ssh_test.py`、`tests/fixtures` |
| 文档 | 本页、`PROGRESS.md`、`README.md` |

## 验证记录

| 平台及命令 | 结果 |
| --- | --- |
| Windows Qt 6.8.3 / MinGW 13.1，`cmake -S . -B build-windows-gui`、`cmake --build build-windows-gui -j 2` | 配置、构建退出 0 |
| Windows `ctest --test-dir build-windows-gui -LE native-desktop --output-on-failure` | 28/28，149.04 秒，包含本机版本页、快速换路径、取消、能力不匹配、在途销毁及当前下载测试 |
| Ubuntu GCC 11.4 / SQLite 3.37.2，专用源码目录中按既有依赖参数配置、`cmake --build build -j 2`、`ctest --test-dir build --output-on-failure` | 52/52，38.15 秒，包含上传、断点续传、发布故障、当前下载及历史迁移 |
| Ubuntu `ctest --test-dir build -R "^version_history_integration$" --output-on-failure` | 在补充旧库当前下载校验及夹具租期处理后，1/1 通过 |
| Windows `windows_gui_history_ssh_test.py` | 对已迁移夹具查到三个历史版本，当前 SHA-256 与旧版本未知时间核对通过 |
| Windows `windows_gui_download_ssh_test.py` | 对 v3 服务端下载空、中文和多块文件，大小及 SHA-256 一致 |

Linux 日志在 `/tmp/chunkharbor-r22-src-WN6VOZ8F` 的 `build-final.log`、`ctest.log` 和 `ctest-initial.log`。Windows 日志在构建目录的 `r2-2-ctest.log`、`r2-2-history-ssh.log`、`r2-2-download-ssh.log`。Ubuntu 首次配置缺少 OpenSSL 头文件路径，随后复用项目依赖及其多架构 include 路径。首轮 CTest 48/52，定位为历史请求的能力判断和重复发布时的触发器唯一约束，修复后重跑。

本轮真实联调命令在 Windows 仓库根执行：

```powershell
python tests/integration/windows_gui_history_ssh_test.py build-windows-gui/chunkharbor-server-history-gui-test.exe miniob-ubuntu /tmp/chunkharbor-r22-src-WN6VOZ8F/build/chunkharbor-server /tmp/chunkharbor-r22-src-WN6VOZ8F/tests/fixtures/r22-v2-backup.tar.gz
python tests/integration/windows_gui_download_ssh_test.py build-windows-gui/chunkharbor-gui-download-probe.exe miniob-ubuntu /tmp/chunkharbor-r22-src-WN6VOZ8F/build/chunkharbor-server
```

原生桌面下拉框专项、MSVC、硬件掉电及真实备份根升级未运行。迁移测试使用夹具副本，覆盖 SQL 错误回滚、提交前进程退出、再次启动、已确认块续传和旧库当前内容下载。旧库已经改名但尚未完成数据库发布的升级边界未用该夹具专项验证。长时间保存的夹具副本在测试中可刷新活跃会话租期，归档和已提交版本数据保持原样。独立只读审查未发现剩余阻塞问题。

下一轮是 R2.3 指定历史版本读取及恢复。
