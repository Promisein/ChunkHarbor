# R2.4 目录快照创建、持久化与查询

快照固定服务器在一次 SQLite 事务内看到的指定逻辑目录下的已提交文件版本集合。请求使用非空逻辑目录根，例如 `docs`。匹配范围为 `docs/` 下的文件，排除 `docs2/` 和同名文件 `docs` 本身。清单中的每一条使用相对路径。快照只写元数据，文件内容继续存放在对应的不可变版本对象中。

## 现有目录语义

| 情况 | v1 快照行为 |
| --- | --- |
| 源文件在扫描期间变化 | 采用现有客户端的单文件扫描、校验及提交结果。快照时点由服务器事务确定 |
| 部分文件成功 | 仅在显式创建请求到达后固定服务器状态。可以混合新提交、原有和未改变版本 |
| 服务器比源端多文件 | 已提交服务器文件仍在集合中 |
| 源端删除、改名 | 当前上传协议不传播删除，旧路径继续保留，改名后的新路径可独立提交 |
| 空目录、无匹配路径 | 扫描器没有目录条目。允许提交零文件快照，无法区分空目录和不存在的逻辑前缀 |
| 并发更新同一路径 | 捕获的版本取决于哪个事务先完成，清单一旦提交即固定 |

若后续需要源目录清单语义，可以新增独立的协商能力及带批次 ID 的有界清单，显式区分文件、空目录和删除标记。批次完成时再核验所有文件路径及版本引用并发布。这可以保留本轮服务端集合语义，也给“部分失败是否允许发布”留出明确的策略入口。

## 数据结构与一致性

- `directory_snapshots` 保存随机 128 位 ID、逻辑根、UTC 创建秒、稳定序号、状态及文件数。状态从 `creating` 变为 `committed`。
- `snapshot_entries` 以快照 ID 和二进制排序的相对路径为主键，引用 `version_history` 中已发布的版本。第二个外键指向 `versions` 或尚未整理的已提交 `transfers`。后者的会话 ID 同时是不可变对象 ID。大小和摘要从相应版本记录查询，无需为每个快照复制内容或摘要。
- 创建使用现有数据库互斥锁及 `BEGIN IMMEDIATE`。在同一写事务内创建快照头，按路径范围通过 `INSERT … SELECT` 固定当前文件及已提交旧上传，最后更新计数和状态后提交。现有 `current_files` 对同路径旧上传优先。历史记录必须匹配完整逻辑路径，异常版本引用、大小或摘要会使整次创建失败。
- SQLite 同时只允许一个写事务。备份提交在快照完成之前或之后推进当前指针。快照头与条目同时可见。SQL 报错或在提交前终止会回滚整个事务，创建中的记录也会被查询过滤。失败或中止由调用结果体现，不持久化可恢复记录。若连接在 COMMIT 后、回复前断开，客户端可通过列表确认是否已经创建。再次发送创建请求会产生新快照。 [SQLite 事务](https://www.sqlite.org/lang_transaction.html)
- 外键约束在服务端开启时阻止直接删除被引用的数据库记录。R4 清理还需要在删除磁盘对象前检查这些引用。对象可用性与哈希继续由指定版本下载验证，创建快照只检查提交元数据。 [SQLite 外键](https://www.sqlite.org/foreignkeys.html)

单个快照最多 10,000 个文件。创建不读取文件正文。服务端 SQL 工作经既有有界磁盘工作池执行。快照列表索引为根路径和降序序号，条目按主键范围向后查找，各查询多取一条来确定是否还有下一页。

## 协议

客户端通过 HELLO 声明 `directory_snapshot_v1`。当前文件下载、指定版本下载、历史查询继续使用原有能力及消息。

| 请求帧 | 载荷 | 响应 |
| --- | --- | --- |
| 37 创建 | `{root}` | 39 快照头 |
| 38 元数据 | `{snapshot_id}` | 39 快照头 |
| 40 列表 | `{root,before,limit}` | 41 `{root,snapshots,next_before}` |
| 42 条目 | `{snapshot_id,after_path,limit}` | 43 `{snapshot_id,entries,next_after_path}` |

快照头包含 `snapshot_id`、`root`、`sequence`、`created_at_s`、`state`、`file_count`。列表只在外层返回 `root`。列表默认 32、最多 64 条，首屏 `before=null`，后续使用独占的序号上界。新快照在当前游标之前出现，刷新首屏可看到。条目按相对路径升序，每条含 `path`、`version_id`、`size`、`sha256`，首屏 `after_path=""`，每页最多 6 条。末页游标分别为 null 和空字符串。逻辑路径上限 4096 UTF-8 字节，控制帧上限 64 KiB。即使路径含大量引号，六条加一个重复游标也有明确的编码上界。

未知或未提交快照返回 `SNAPSHOT_NOT_FOUND`，非法字段返回 `MALFORMED_MESSAGE`，超过单快照文件上限返回 `QUOTA_EXCEEDED`。本轮客户端入口为共享协议编解码和可运行的协议测试探针。

## 迁移与验证

schema v3→v4 在独立事务中新增两张表和索引，同时推进两个 schema 版本标记。升级时快照列表为空，已有版本、当前指针和上传检查点保留。测试使用真实 v3 归档的副本，覆盖 SQL 失败、COMMIT 前退出、重启及重复打开。实际备份根升级未运行。 [夹具说明](../tests/fixtures/README.md)

| 修改范围 | 文件 |
| --- | --- |
| 协议 | `DirectorySnapshot.h/.cpp`、`FrameCodec.cpp`、`UploadControl.cpp` |
| 服务端 | `schema_v4.sql`、`SqliteSnapshot.inc`、`SqliteStore.h/.cpp`、`UploadStorage.h/.cpp`、`UploadHandler.cpp` |
| 测试 | `CMakeLists.txt`、`framecodec_test.cpp`、`directory_snapshot_test.cpp`、`directory_snapshot_test.py`、`delta_crash_hook.cpp`、`version_history_test.py`、`windows_gui_history_ssh_test.py`、v3 夹具 |
| 文档 | 本页、`PROGRESS.md`、`README.md`、夹具说明 |

旧 R2.3 服务端在能力断言处按预期失败。首轮 Linux CTest 54/55，新增测试误将列表外层根路径当作每行字段。修正断言后，快照专项通过。测试还验证了中途 SQL 报错、提交前 SIGKILL、并发备份等待、70 条真实上传分页、10,001 条合成元数据拒绝、长路径响应上限、引用保护、空清单、源端删除后服务端仍保留和未提交状态过滤。

Ubuntu 隔离源码目录为 `/tmp/chunkharbor-r24-src-SWOSq2dr`，沿用既有 OpenSSL/SQLite 依赖参数。Windows 沿用 Qt 6.8.3 / MinGW 13.1 构建目录。构建与测试命令为 `cmake --build build -j 2`、`ctest --test-dir build --output-on-failure`，以及 `cmake --build build-windows-gui -j 2`、`ctest --test-dir build-windows-gui -LE native-desktop --output-on-failure`。

| 验证 | 实际结果 |
| --- | --- |
| Ubuntu GCC 11.4 配置、构建与完整 CTest | **57/57**，41.27 秒。补充第二个 SQLite 写连接无法抢占事务的断言后，故障专项再跑 1/1 |
| Windows Qt 6.8.3 / MinGW 13.1 配置、构建及非原生 CTest | **30/30**，145.71 秒 |
| Windows → SSH → Ubuntu 快照 | 创建 `d1152048f5e355cfe38b94962dbbd5c1`，列表、元数据、逐条分页及两条摘要均核对通过 |
| Windows Qt → SSH → Ubuntu 历史恢复 | v4 服务端恢复 A、B、C，内容和 SHA-256 一致 |
| Windows Qt → SSH → Ubuntu 当前下载 | 空文件、中文文件和多块文件大小及 SHA-256 一致 |

Ubuntu 日志在隔离源码目录的 `build-delivery.log`、`ctest-delivery.log`、`ctest-initial.log`。Windows 日志在构建目录的 `r2-4-build-final.log`、`r2-4-ctest-final.log`、`r2-4-snapshot-ssh.log`、`r2-4-restore-ssh.log`。本轮使用的真实 SSH 命令如下。

```powershell
python tests/integration/windows_gui_history_ssh_test.py tests/integration/directory_snapshot_test.py miniob-ubuntu /tmp/chunkharbor-r24-src-SWOSq2dr/build/chunkharbor-server /tmp/chunkharbor-r24-src-SWOSq2dr/tests/fixtures/r24-v3-backup.tar.gz --snapshot
python tests/integration/windows_gui_history_ssh_test.py build-windows-gui/chunkharbor-gui-download-probe.exe miniob-ubuntu /tmp/chunkharbor-r24-src-SWOSq2dr/build/chunkharbor-server /tmp/chunkharbor-r24-src-SWOSq2dr/tests/fixtures/r24-v3-backup.tar.gz --restore
python tests/integration/windows_gui_download_ssh_test.py build-windows-gui/chunkharbor-gui-download-probe.exe miniob-ubuntu /tmp/chunkharbor-r24-src-SWOSq2dr/build/chunkharbor-server
```

R1 上传与续传、R2.1 当前下载、R2.2 历史查询、R2.3 指定版本下载均包含在相关回归中。并发专项在快照提交点前暂停，验证事务对其他读取连接不可见，并拒绝第二个 SQLite 写事务。同时发起上传提交，释放后分别核对快照中的旧版本和之后快照中的新版本。服务端锁等待点本身没有独立测试钩子。

当前下载日志为 `r2-4-current-ssh.log`。提交后应答丢失、真实硬件掉电、原生下拉框专项、MSVC 以及真实备份根升级未运行。R2.4 停在服务端快照与协议能力，R2.5 负责目录恢复交互及 Windows 保留名、大小写冲突等本地路径校验。
