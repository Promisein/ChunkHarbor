# R2.5.1 Windows 目录快照浏览与恢复预览

打开侧栏「目录快照」，输入服务端逻辑目录，点击「查询快照」。列表显示快照 ID、创建时间、文件数和逻辑根，支持查看更早快照。选择一行后，客户端先核对快照头，再读取完整文件清单。清单读完后显示文件数、总字节数、相对路径、版本 ID 和 SHA-256，可按本地展示页翻阅。选择本地目录后，还会显示预期目标路径、路径校验和目标冲突。

这是只读预览。服务端在 SQLite 的同一事务中提交 `directory_snapshots` 快照头及 `snapshot_entries` 清单。每个条目将相对路径绑定到 `version_history` 中的不可变版本，并引用已有版本对象或旧上传记录。查询只返回已提交快照，`current_files` 后续移动不会改变条目。源端空目录和删除标记没有独立记录，源端删除或改名后旧路径可能仍保留。快照的服务器事务时点也不代表 Windows 源目录的统一时间点。 [R2.4 存储与协议](R2_4_DIRECTORY_SNAPSHOT.md)

## 完整性、分页与内存

客户端协商 `directory_snapshot_v1`，使用 40/41 查询快照列表，38/39 核对所选快照的元数据，42/43 读取清单。每个查询任务使用独立的 QtTcpTransport。清单任务在同一连接上依序发送请求，同时最多一个在途请求。每页检查快照 ID、升序相对路径、请求游标、累计行数及末页行数。缺页、提前结束、错误 ID、超限、非法路径和断连会清空未完成结果。读取的都是快照条目对应的版本 ID，预览总量是条目大小的精确整数累加。

R2.4 的 64 KiB 控制帧要容纳 JSON。单个路径允许 4096 字节，引号等转义最坏接近 8192 字节。响应还会重复最后一条路径作为游标，因此六条路径和游标最多接近 56 KiB，再为版本 ID、哈希、大小及结构字段留余量。服务端正常填满每个非末页时，10,000 条要进行 1,667 次清单请求。若往返时间为 20 ms，光是这部分往返就约 33 秒，另有数据库、解析和预检时间。

后续最小兼容优化可以独立协商按**实际编码字节预算**装页，同时限制条数，继续严格使用 64 KiB 帧上限。短路径可在同一页放入更多行，最长路径仍受字节预算约束，旧客户端继续用六条页。本轮沿用现有协议和上限。

UI 列表最多请求 32 条，清单最多保留快照头声明的 10,000 条。原始 UTF-8 路径最坏约 39.1 MiB，另有每条的版本和摘要及容器开销。完成后将清单移交模型，本地展示页为 100 行，目标路径按显示需求计算。文件属性预检共享只读清单。切换时已取消的后台任务可能暂时保留一份旧清单，最多一个预检线程和一份旧任务数据。

## 线程与目标安全

网络由 Qt 事件循环异步驱动。切换快照、修改根路径、修改端点或取消时，旧连接断开，半成品清空。回调检查所属连接，预检回调还检查任务修订号。关窗取消旧任务并等待预检线程结束，Windows 正在进行的一次属性查询可能延迟退出。

预检接受 Windows 本地盘的绝对目录。逐组件拒绝绝对或穿越形式的相对路径、保留名、非法字符、尾随点和空格，以及达到 240 个 UTF-16 单元的目标。用大小写折叠检查同名文件、目录拼写别名和文件与目录的前缀冲突。逐组件读取文件属性，发现重解析点便停止该路径的检查，并报告已有目标、被文件占用的父目录或无法确认的属性。检测到问题的文件保留在预览中。 [Windows 文件命名](https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file) · [GetFileAttributesW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfileattributesw)

属性读取只是当时的观察。预检不会建立目录、写入文件或占用目标，也不固定文件系统对象。名称碰撞检查基于大小写折叠的拼写，现有目录的 8.3 短名与长名可能被视为两条路径。R2.5.2 执行恢复时仍需按实际文件系统重新检查命名、别名、重解析点和路径竞争，并用句柄保护写入及防覆盖发布。零文件快照显示明确提示。 [Windows 短名称和长名称](https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file#short-vs-long-names)

## 修改文件

| 范围 | 文件 |
| --- | --- |
| GUI | `SnapshotPage.h/.cpp`、`SnapshotPreflight.h/.cpp`、`MainWindow.cpp` |
| 构建与验证 | `CMakeLists.txt`、`snapshot_page_test.cpp`、`gui_pages_test.cpp`、`windows_gui_history_ssh_test.py` |
| 文档 | 本页、`PROGRESS.md`、`README.md` |

## 验证

测试先在旧窗口上复现快照入口缺失。新增对端覆盖空快照、单文件、多层及中文路径、多页、错误快照 ID、截断、断连、旧能力缺失、切换和关窗。Windows 临时目录验证已有目标、大小写碰撞、文件与目录冲突、保留名、非法字符、超长路径和目录 junction，核对预检没有改写原文件或建立目录。10,000 项测试通过真实 Qt Socket 和 1,667 次有界拉取验证数量及本地展示分页。测试耗时包含事件驱动的测试夹具等待与属性预检，不作为 WAN 性能基准。

- Windows Qt 6.8.3 / MinGW 13.1 下 `cmake --build build-windows-gui -j 2` 退出 0，`ctest --test-dir build-windows-gui -LE native-desktop --output-on-failure -j 2` **31/31**，93.96 秒。快照专项 28.28 秒，其中 10,000 项环回用例 26.90 秒。
- Ubuntu 通过 SSH 在 R2.4 的隔离构建上运行 `ctest --test-dir build --output-on-failure`，**57/57**，43.86 秒。R2.5.1 沿用该构建的服务端，协议和存储未变更。
- Windows 通过 SSH 隧道查询隔离 Ubuntu 服务端。联调将 v3 真实测试夹具副本升级到 v4，创建快照 `76f5baa40377c9b30a5ff7b07b2265e6` 后对同一路径提交新版本。Qt 预览读到固定的 2 个文件、旧版本 ID `72efe174559bd35db95e227abdf7ae99` 和 SHA-256 `7a2fb88ecfd47ffd4d45cd3aac517bf90539a7aaa5eee908028f30b6a581828f`，且目标目录无新文件。两条夹具记录共 28 字节。

```powershell
python tests/integration/windows_gui_history_ssh_test.py build-windows-gui/chunkharbor-snapshot-page-test.exe miniob-ubuntu /tmp/chunkharbor-r24-src-SWOSq2dr/build/chunkharbor-server /tmp/chunkharbor-r24-src-SWOSq2dr/tests/fixtures/r24-v3-backup.tar.gz --snapshot-preview
```

Windows 原生目录选择框、原生桌面下拉框专项、MSVC、远端 10,000 项延迟和预检线程在系统调用中挂起时的关窗延迟未验证。规模测试使用 Windows 本机 Qt 对端。

本机日志保存在 `build-windows-gui/r2-5-1-build-final.log`、`build-windows-gui/r2-5-1-ctest-final.log`、`build-windows-gui/r2-5-1-preview-ssh-final.log`。Ubuntu 日志在隔离构建根的 `ctest-r251-regression.log`。
