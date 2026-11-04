# R2.5.3 Windows 当前文件夹下载

打开「目录快照」，输入服务端逻辑目录，点击「下载文件夹」。确认服务端会保留一份持久快照后，等待完整清单。选择本机已有父目录和新目录名，检查文件数、字节数及冲突，再点「下载到新目录」确认。传输栏显示逻辑目录、快照 ID、当前文件、已校验文件数、已下载字节和执行状态。

历史恢复直接选择已有快照。当前文件夹下载增加一次明确授权的服务端创建步骤，之后走同一套清单、预检和恢复流程。下载失败后也可从历史列表选择已确认的快照再次恢复，继续使用原来的版本集合。独立空目录和源端删除标记没有入库，已从源端删除但仍在服务器上的路径可能出现。空目录和不存在的前缀都显示「没有已提交文件」。服务端已创建的空快照保留，此次下载不会创建本地目录。历史快照恢复保留原有零文件行为。

## 固定版本与处理丢应答

- `SnapshotPage::confirmCapture` 先通过 `directory_snapshot_v1` 列出同根最新一条快照作为序号基线，再发送一次 CREATE（37）。服务端在 SQLite 的同一事务内固定当前已提交文件集合并提交快照。逻辑范围为 `root/` 下的文件，`photos/a/` 不包含 `photos/abc/`。界面允许输入一个末尾斜杠，并用协议校验逻辑路径。
- CREATE 返回有效已提交快照头后，客户端绑定该 ID，核对元数据并按游标拉取清单。只有全部页完整且计数一致才可确认执行。新增提交只更新当前映射，计划中的不可变版本 ID 和摘要保持原值。历史对象缺失、损坏或校验失败会终止任务。
- 请求 37 发出后，连接中断、超时、异常应答或用户停止等待会触发一次原端点的只读 LIST 核对，最多读取最新的 32 条。当前协议没有客户端操作 ID。高于基线的序号只能作为候选，即使仅有一条也无法确定归属。客户端不自动选中候选或重发 CREATE。如果没有新增候选或无法查询，界面提示结果未确认，用户可稍后查看原服务器的历史快照。
- 关闭窗口会关闭确认框并取消尚未发出创建的基线查询。切换端点也立即取消基线查询，即使随后切回原端点。已发出创建请求的流程在关窗时会等待确认或只读核对，结果不确定时保留窗口。取消本地预览或传输不撤销服务端已提交快照。强制退出时可能留下结果未知的持久快照，应按逻辑根查看快照列表。

## 复用的恢复与安全发布

`SnapshotPreflight` 生成 Windows 目标检查，`SnapshotRestore` 固定端点和清单并串行调用 `historical_download_v1`。每个文件验证元数据、分块和整文件 SHA-256，刷盘后计入完成数。保留单文件 64 MiB、数据块 256 KiB、单快照 10,000 文件及单页最多六条。没有服务端 schema 或消息语义变更。

`SnapshotOutput` 沿用 R2.5.2 的本机固定 NTFS 暂存树和发布。按句柄创建文件与目录，最终核对暂存树和路径，使用一次同卷、拒绝覆盖的重命名。竞争目标由系统裁决。失败只处理本任务拥有的对象，清理失败会显示暂存路径。目录发布和文件刷新不构成掉电事务。同属主及管理员主动篡改的边界与 R2.5.2 一致。 [R2.5.2 实现细节](R2_5_2_SNAPSHOT_RESTORE.md)

## 修改文件

| 文件 | 内容 |
| --- | --- |
| `apps/gui/pages/SnapshotPage.h/.cpp` | 当前文件夹入口、持久快照确认、基线和只读核对、复用清单与预览 |
| `apps/gui/MainWindow.cpp` | 协调创建期间的关窗 |
| `apps/gui/SnapshotRestore.cpp` | 执行过程中显示发布阶段 |
| `tests/unit/snapshot_page_test.cpp` | Windows GUI 环回测试及真实 SSH 探针 |
| `tests/integration/windows_gui_history_ssh_test.py` | 隔离夹具、实际上传变更、当前文件夹联调 |
| `docs/PROGRESS.md`、本页 | 交付与验证记录 |

## 实际验证

- 新入口用例先在旧 GUI 上按预期失败。Windows Qt 6.8.3 / MinGW 13.1 执行 `cmake --build build-windows-gui -j 2` 成功，`ctest --test-dir build-windows-gui -LE native-desktop --output-on-failure -j 2` **31/31**，106.28 秒。专项覆盖取消持久快照确认、确认框和基线阶段关窗、端点切走再切回、创建应答丢失后两条并发候选、重复点击、空结果、非空分页和串行发布。回归还覆盖历史快照恢复、万项清单、目标竞争、校验失败、断连、取消和关窗。
- Ubuntu 复用隔离的 R2.4 构建，`ctest --test-dir build --output-on-failure` **57/57**，40.93 秒。服务端二进制无需重编。
- Windows 经 SSH 对隔离 v3 夹具升级后的 v4 服务端执行当前文件夹下载。GUI 自行创建快照 `265eab6860de52e7502cd3b460e31a6f`。测试转发器在第一次清单请求和第一次数据块请求处分别等待服务端提交 `DDDD` 和 `EEEE`，然后继续转发。服务端快照列表最终只有本次的一条记录，三条映射未改变。Windows 发布 3 个文件共 34 字节，相邻前缀 `fixture-extra` 未进入本地目录。

| 下载文件 | 本地 SHA-256 |
| --- | --- |
| `file.txt`（旧版 C） | `7a2fb88ecfd47ffd4d45cd3aac517bf90539a7aaa5eee908028f30b6a581828f` |
| `legacy.txt` | `cb6299c02d74eae6eab621fd6ae59623b38909fd841cc5a19cc2a25ceb60a1e7` |
| `子目录/文件.txt` | `233562de1a0288b139c4fa40b7d189f806e906eeb048517aeb67f34ac0e2faf1` |

```powershell
python tests/integration/windows_gui_history_ssh_test.py build-windows-gui/chunkharbor-snapshot-page-test.exe miniob-ubuntu /tmp/chunkharbor-r24-src-SWOSq2dr/build/chunkharbor-server /tmp/chunkharbor-r24-src-SWOSq2dr/tests/fixtures/r24-v3-backup.tar.gz --current-folder
```

Windows 日志在构建目录的 `r2-5-3-build-final.log`、`r2-5-3-ctest-final.log` 和 `r2-5-3-ssh-final.log`。Ubuntu 日志在隔离构建目录的 `ctest-r253-regression.log`。

丢创建应答的用例使用本机可控对端。真实 SSH 丢应答、强制结束进程或掉电、MSVC、原生目录选择手工操作、原生桌面下拉框专项和远端万文件完整下载未验证。
