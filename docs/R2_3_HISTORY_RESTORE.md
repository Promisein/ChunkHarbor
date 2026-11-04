# R2.3 指定历史版本下载与安全恢复

Windows 操作顺序为「版本历史」→「查看服务器历史」→ 查询逻辑路径 → 选中一行 →「下载此版本」→ 选择本地保存位置。保存框和窗口底部传输栏会展示完整版本 ID，传输栏显示字节进度、校验结果和取消入口。路径、端点、版本 ID、大小和 SHA-256 在打开保存框前固定。切换版本、查询其他文件或离开历史页时，进行中的下载继续使用这份选择。

## 服务端对象与协议

- `version_history` 证明版本已发布。请求以逻辑路径与不可变版本 ID 同时匹配该表，再从 `versions` 读取大小和整文件 SHA-256。尚未整理入 `versions` 的已提交旧上传从匹配的 `transfers` 行读取元数据。
- 字节来自备份根下的 `.versions/<版本 ID>/data`。版本 ID 必须是 32 个小写十六进制字符。文件路径以备份根 FD 为锚点安全打开，复用符号链接、挂载点、文件类型、属主、权限及硬链接限制。已登记对象按既有版本生命周期保留。
- 服务端以 RAII 持有已打开对象的文件描述符，核对文件长度和完整 SHA-256，之后按请求偏移使用 `pread`。当前版本切换只改变 `current_files` 指针与当前文件，已打开的历史对象由文件描述符固定。查询和读取在有界磁盘工作池执行，下载连接释放时关闭描述符。
- HELLO 新增 `historical_download_v1`，使用帧 36，载荷为 `{path,version_id}`，版本 ID 必填。沿用 READY 32、GET_CHUNK 33 和数据块 14，一条连接只绑定一个下载对象。文件上限 64 MiB，数据块上限 256 KiB，控制帧上限 64 KiB。
- `download_v1` 仍使用帧 13，版本 ID 是当前版本匹配约束。旧客户端可继续协商原能力。历史请求按发布记录取对象，`current_files` 和 schema v3 无需修改。恢复只写用户选择的 Windows 本地路径。
- 未发布、未知或路径与 ID 不匹配时返回 `VERSION_NOT_FOUND`。不可读取的对象或无效元数据返回 `VERSION_UNAVAILABLE`，对象大小或摘要与元数据不匹配时返回 `CHECKSUM_MISMATCH`。历史列表为元数据视图，数据对象可用性在开始下载时核验。
- 传统上传先完成不可变副本的校验、刷新和发布，再把 SQLite 状态从 COMMITTING 推进到 COMMITTED。历史触发器随后登记可见记录。若进程在副本刷盘阶段退出，启动修复用已发布的当前文件补齐对象，再登记提交。

## Windows 安全发布

历史页和当前文件页使用同一个窗口持有的 DownloadController。Qt Socket、哈希和磁盘写入属于同一下载线程，GUI 通过有界的 queued 回调更新。分块校验后写入目标同目录的独占临时文件，整文件校验及 FlushFileBuffers 成功后，以原有 Windows 句柄重命名操作发布，系统负责拒绝覆盖竞争目标。取消、断连、写入错误和校验失败走原 RAII 清理路径。取消与最终发布以原子状态转换决定先后，已进入发布阶段的任务按实际发布结果报告。

## 变更文件

| 范围 | 文件 |
| --- | --- |
| 协议与服务端 | `Download.h/.cpp`、`FrameCodec.cpp`、`UploadControl.cpp`、`UploadHandler.cpp`、`UploadStorage.h/.cpp`、`DownloadStorage.inc` |
| Windows GUI | `DownloadController.h/.cpp`、`VersionsPage.h/.cpp`、`FilesPage.cpp`、`MainWindow.cpp` |
| 测试 | `CMakeLists.txt`、`framecodec_test.cpp`、`history_restore_test.py`、`upload_delay_hook.cpp`、`server_history_gui_test.cpp`、`gui_download_probe.cpp`、`gui_download_test.py`、`windows_gui_history_ssh_test.py` |
| 文档 | `README.md`、`PROGRESS.md`、本页 |

## 实际验证

Linux 在专用源码目录 `/tmp/chunkharbor-r23-src-sdG0OldV` 按既有 Ubuntu 依赖参数配置。GCC 11.4 执行 `cmake --build build -j 2` 退出 0，最终 `ctest --test-dir build --output-on-failure` **54/54** 通过，41.63 秒。历史专项使用 R2.2 保存的同一 v2 归档，检查迁移后的 A/B/C、旧上传、当前版本发布期间已固定的 A，以及空文件、中文路径、多块数据、错路径、未知 ID、缺失对象、符号链接、对象损坏和旧客户端。最终日志为 `build-delivery.log` 和 `ctest-delivery.log`。

Windows Qt 6.8.3 / MinGW 13.1 使用 `cmake -S . -B build-windows-gui`、`cmake --build build-windows-gui -j 2` 配置构建退出 0。最终 `ctest --test-dir build-windows-gui -LE native-desktop --output-on-failure` **29/29** 通过，143.91 秒。14 项历史页面场景覆盖空文件、多块、选择切换、两级哈希错误、断连、传输中取消、已有目标、目标竞争、ID 不匹配、关窗、直接销毁、对象不可用及能力不匹配。日志为 `build-windows-gui/r2-3-build-final.log` 和 `r2-3-ctest-final.log`。

旧 R2.2 服务端在新能力断言上按预期失败。首轮 Linux 回归 52/53，首轮 Windows 回归 28/29。历史错误用例定位到新增错误码未加入共享错误帧校验表。审查另发现历史页检查了已移动的响应对象，现已改为按实际显示的数据判断空页。传统上传的发布顺序专项在不可变副本刷盘前暂停，先复现了 COMMITTED 提前可见，再验证修复后的正常完成与进程终止后启动恢复。原始失败日志保留。

真实 Windows→SSH→Ubuntu 使用 v2 归档副本，GUI 按历史页选中版本下载到 Windows 临时目录。每个版本均为 14 字节，内容与生成夹具时一致：

| 版本 | ID | 服务端与 Windows 一致的 SHA-256 |
| --- | --- | --- |
| A | `0cc6a79ead525d61d1ec0c306ff662e2` | `420dbf371827b6075d7aa33200540df7bddc39ed00e5ee982486061678523c21` |
| B | `2978b3f0bf258a4d0e3029994f2bae73` | `bd71325ea42140323bf1ce3b3f78b488a4b26f6c3edda9438f45ad96fc3bc468` |
| C | `72efe174559bd35db95e227abdf7ae99` | `7a2fb88ecfd47ffd4d45cd3aac517bf90539a7aaa5eee908028f30b6a581828f` |

R2.1 当前下载通过同一新版服务端做了真实 SSH 回归，空、中文和多块文件大小与 SHA-256 一致。R1 真实 SSH 上传和续传矩阵 12 组通过。Linux 上传、持久续传、发布故障及 R2.2 历史查询在完整 CTest 中通过。夹具及真实备份根相互独立。

最终联调在 Windows 仓库根执行以下命令，输出分别记录在构建目录的 `r2-3-history-delivery.log`、`r2-3-current-delivery.log` 和 `r2-3-upload-delivery.log`。历史测试副本为 `/tmp/chunkharbor-history-ssh-Hv0qcR5k`。

```powershell
python tests/integration/windows_gui_history_ssh_test.py build-windows-gui/chunkharbor-gui-download-probe.exe miniob-ubuntu /tmp/chunkharbor-r23-src-sdG0OldV/build/chunkharbor-server /tmp/chunkharbor-r23-src-sdG0OldV/tests/fixtures/r22-v2-backup.tar.gz --restore build-windows-gui/r2-3-restore.png
python tests/integration/windows_gui_download_ssh_test.py build-windows-gui/chunkharbor-gui-download-probe.exe miniob-ubuntu /tmp/chunkharbor-r23-src-sdG0OldV/build/chunkharbor-server
python tests/integration/windows_mode_ssh_test.py build-windows-gui/chunkharbor-gui-transfer-probe.exe miniob-ubuntu /tmp/chunkharbor-r23-src-sdG0OldV/build/chunkharbor-server
```

测试范围保留 64 MiB、单任务、本地盘路径和不覆盖约束。文件描述符固定的是已打开对象，不能阻止外部进程原地改写数据。完整 SHA-256 不匹配时 Windows 拒绝发布。自动化保存框使用 Qt 非原生对话框。Windows 原生保存框手动交互、原生下拉框专项、MSVC、硬件掉电及真实备份根升级未验证。
