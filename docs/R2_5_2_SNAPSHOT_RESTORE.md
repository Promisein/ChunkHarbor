# R2.5.2 指定快照的 Windows 整目录恢复

在「目录快照」中查询服务端逻辑目录，选中快照并等待清单核验完成。选择已存在的本地父目录，输入一个新目录名，检查逐项路径、文件数和总字节数，再点「恢复快照」确认。恢复栏显示当前文件及其字节进度、整目录的已校验文件数和累计已下载字节。全部文件完成刷盘、目录核验并发布后才显示成功。零文件快照可发布为空目录。

恢复仍采用 R2.4 的服务端已提交版本集合。服务端没有独立空目录和源端删除标记，也不记录 Windows 源目录的严格统一时间点。恢复只创建新目录。

## 快照绑定、网络与线程

- 从 `directory_snapshot_v1` 读完 R2.5.1 的六条清单页后，控制器捕获端点、快照头、`shared_ptr<const Manifest>` 和目标。确认窗口打开后如果预览修订号改变，旧确认失效。恢复开始后目录名、快照选择和重复确认在页面上被锁定。
- 串行用一个工作线程执行，单个文件的远端路径由逻辑根和清单相对路径拼接。复用 `receiveDownload` 执行 R2.3 的 `historical_download_v1`，READY 的路径、版本 ID、大小和 SHA-256 必须与清单匹配。没有查询当前版本或修改服务端指针的请求。历史版本缺失或损坏时整个任务失败。
- 每个连接只读一份历史对象，每次最多拉取一个 256 KiB 数据块。块先校验再写，整文件校验和 `FlushFileBuffers` 成功后完成计数加一。文件仍受 64 MiB 上限约束。Socket、文件句柄和磁盘操作都属于后台线程。GUI 的进度投递至多挂起一个事件，任务结束以独立终态事件收尾。
- C++ RAII 将句柄、套接字与任务生命周期绑定。取消和开始发布竞争同一次原子状态转换。取消先到便清理暂存；发布先到则报告系统调用的实际结果。关窗先取消并等终态，直接销毁页面时先断开界面回调，再等待工作线程。

## Windows 路径、暂存与发布

恢复执行限定在 Windows 本机固定 NTFS 盘。路径复核拒绝穿越、保留名、非法字符、大小写及文件与目录冲突，目标路径须短于 240 个 UTF-16 单元。以盘根为起点逐级打开父目录，持有禁止共享删除的句柄。父路径的实际长名必须匹配用户选择，8.3 短名和其他别名被拒绝。

在固定的目标父句柄下独占创建随机 `.chunkharbor-restore-<UUID>`，DACL 将访问限制到属主与 SYSTEM。后续每个组件用 `NtCreateFile` 相对父句柄操作，要求不解析重解析点。新成员使用 `FILE_CREATE`，若大小写别名、短名或其他任务占用了拼写，创建失败。打开曾创建的父目录时核对 NTFS 文件 ID、类型和重解析属性。持有本文件的整条父链直到成功创建文件。文件写入和刷盘通过原始句柄完成。

发布前枚举整树，核对所有条目的名称、文件 ID、类型、大小及文件硬链接数，发现额外成员时中止。单文件句柄保持独占直到最后，NTFS 目录重命名要求先关闭子文件句柄。随后用 `NtSetInformationFile(FileRenameInformation)` 将暂存根句柄相对于同一个已固定的父句柄重命名，`ReplaceIfExists=FALSE`。目标竞争由内核裁决，无覆盖和逐文件发布回退。该操作提供 NTFS 下的同卷单次命名空间发布。文件刷新与目录重命名没有组成跨操作的掉电事务。 [NtCreateFile](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntcreatefile) · [目录重命名规则](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_rename_information)

失败或取消时优先用持有的对象句柄删除，需重开时只接受暂存树记录的名称、文件 ID 与类型。只处理本任务创建的条目，未知或已换对象的成员保留。若无法清空，界面和日志显示暂存路径。异常退出残留保留在暂存命名空间，应用不会按前缀扫描删除。用户可在确认任务已停止、内容归属和路径后手动处理。暂存目录的 ACL 隔离其他账号。对拥有同一属主身份或管理员权限的主动篡改没有隔离保证，尤其是关闭文件句柄与重命名之间的窗口。选定父目录的文件系统身份从执行时固定。

## 修改文件

| 范围 | 文件 |
| --- | --- |
| 传输 | `apps/gui/DownloadTransfer.h`、`DownloadController.cpp` |
| 目录恢复 | `apps/gui/SnapshotRestore.h/.cpp`、`SnapshotOutput.h/.cpp`、`SnapshotPreflight.h/.cpp` |
| GUI | `apps/gui/pages/SnapshotPage.h/.cpp`、`apps/gui/MainWindow.h/.cpp` |
| 构建与验证 | `CMakeLists.txt`、`tests/unit/snapshot_page_test.cpp`、`tests/integration/windows_gui_history_ssh_test.py` |
| 文档 | `docs/PROGRESS.md`、本页 |

## 实际验证

Windows Qt 6.8.3 / MinGW 13.1 执行 `cmake --build build-windows-gui -j 2` 成功，`ctest --test-dir build-windows-gui -LE native-desktop --output-on-failure -j 2` **31/31**，92.63 秒。恢复专项 36.04 秒，覆盖空快照、单文件、129 文件多页串行恢复、中文多层路径、10,000 项清单的 1,667 次分页、目标已存在及竞争、8.3 父路径别名、重解析点、外来暂存成员、分块与整文件校验失败、断连、取消、重复点击和关窗。R2.5.1 的分页与路径冲突用例和既有上传、单文件下载及版本历史用例一并运行。

Ubuntu 沿用隔离的 R2.4 服务端构建，执行 `ctest --test-dir build --output-on-failure`，**57/57**，42.14 秒。本轮的变更都在 GUI 和测试侧。

真实 Windows→SSH→Ubuntu 使用 v3 备份夹具的隔离副本，启动时迁移到 v4。先创建旧快照 `40d175328c960e4bda342022d953d59b`，提交 `DDDD` 后创建新快照 `037148136c93b7d0dfac39f5871ff934`。从 Windows GUI 各恢复到一个全新的测试目录。原始内容和本地 SHA-256 如下。

| 快照与文件 | 字节数 | SHA-256 |
| --- | ---: | --- |
| 新快照 `file.txt` | 4 | `3989c4e0b53b03fa44fba6af89eeaa5f4347e8496e934ce81364e132cfca25ed` |
| 旧快照 `file.txt` | 14 | `7a2fb88ecfd47ffd4d45cd3aac517bf90539a7aaa5eee908028f30b6a581828f` |
| 旧快照 `legacy.txt` | 14 | `cb6299c02d74eae6eab621fd6ae59623b38909fd841cc5a19cc2a25ceb60a1e7` |

```powershell
python tests/integration/windows_gui_history_ssh_test.py build-windows-gui/chunkharbor-snapshot-page-test.exe miniob-ubuntu /tmp/chunkharbor-r24-src-SWOSq2dr/build/chunkharbor-server /tmp/chunkharbor-r24-src-SWOSq2dr/tests/fixtures/r24-v3-backup.tar.gz --snapshot-restore
```

新增恢复入口的测试先在旧窗口上按预期失败。首轮 SSH 验证通过下载和目录核验后，Win32 包装接口拒绝父句柄相对重命名，返回 Windows 87。最小探针对同一父句柄验证了 NT 接口可以发布，并在目标竞争创建时返回名称冲突。改用 NT 接口后，真实 SSH 两次目录恢复均成功。

日志位于 Windows 构建目录的 `r2-5-2-build-final.log`、`r2-5-2-ctest-final.log`、`r2-5-2-ssh-final.log` 和 Ubuntu 隔离目录的 `ctest-r252-regression.log`。

真实盘满、突然掉电、MSVC、原生目录选择框手工操作、原生桌面下拉框专项和远端万文件完整恢复未验证。测试使用专用临时目录和测试备份夹具。
