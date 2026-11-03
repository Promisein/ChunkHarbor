# ChunkHarbor 项目现状审计（2026-11-03）

本轮仅审计、文档与优先级整理，没有实现 R1–R8 业务代码或改动 CMake/协议/数据库。源码静态检查、历史验收和本轮实际验证分开记录；后续计划见 [ROADMAP_V3](ROADMAP_V3.md)。

## 版本与工作区

审计开始时 Windows `git status --short --branch` 为干净的 `codex/m5-gui`；HEAD 为 `31e5d3a7f2ea6235ba363cebc2001b06e0d08c79`。`origin/main` 本地跟踪引用相同，Windows 本地 `main` 仍在 `db09608`（M4），因此不能以本地 main 分支判断当前源码进度。

本轮实际只读运行：

```text
git status --short --branch
git log -8 --oneline
git branch -vv
git rev-parse HEAD origin/main main
git ls-remote origin refs/heads/main
ssh -o BatchMode=yes -o ConnectTimeout=8 miniob-ubuntu 'cd /home/ning/ChunkHarbor && git status --short --branch && git rev-parse HEAD && git log -3 --oneline && cmake --version | head -1 && ctest --test-dir build -N | tail -3'
```

GitHub main 与 Ubuntu 原工程 `/home/ning/ChunkHarbor` HEAD 均为完整 SHA `31e5d3a7f2ea6235ba363cebc2001b06e0d08c79`；Ubuntu main 工作区干净。此为提交/工作区一致性核对，未逐字节核验忽略文件、依赖、用户数据或常驻进程二进制。不自动切分支、同步、提交或 push。

最近历史：`c6398e2` M1 → `3bed144` M2 → `dbcfddf` M3 → `db09608` M4 → `a64235d` M5 → `b6b62ab` 部分 M6 → `31e5d3a` 发布同步文档。M6 不因提交名称或发布而整体完成。

## 功能与证据矩阵

“已实现”表示有源码与对应测试，不代表无缺陷、全部平台或全部规格验收完成。

| 范围 | 实际状态与源码入口 | 验证证据与缺口 |
| --- | --- | --- |
| M0 工程/平台边界 | 已实现；CMake 划分 protocol/client_core、Linux net/server_core、server/CLI、Qt GUI；另有 portable hash 与 GUI 静态库 | 本轮 Windows GUI 配置构建、Ubuntu server/CLI 构建成功；未新测所有编译器 |
| M1 网络与帧 | 已实现自主单线程 LT epoll、eventfd 投递、Buffer/TcpConnection、半包粘包、部分写/背压/空闲和连接上限；32 字节显式大端 FrameCodec | 本轮 Windows 协议回归通过；100 连接/FD/网络异常见 M1 历史及 network_test.py，本轮未重跑，不是压测结论 |
| M2 首次上传 | 已实现 HELLO、块/整文件 SHA-256、暂存、安全路径和发布，upload 保护已有目标；UploadHandler/UploadStorage/TempFileStore | 本轮 Linux 列表/下载集成含真实 upload；详细写入与故障注入为历史 M2 验收 |
| M3 持久续传/恢复 | 已实现 SQLite WAL + synchronous=FULL、服务端持久会话与客户端状态关联、查询缺块、启动核验/隔离、发布状态协调 | 本轮下载测试重启场景通过；SIGKILL/续传/SQLite 故障完整覆盖见 M3/M5 历史，本轮未重跑全部 |
| M4 固定块增量/预览 | 已实现分页 manifest/plan、固定索引哈希比较、不可变版本/current 指针、Exact 条件发布、目录扫描/排除和顺序批任务 | 本轮 planner/client_delta/scanner 与 GUI worker 回归通过，Linux 下载集成含 delta；不支持 FastCDC/跨偏移复用 |
| M5 GUI 备份 | 已实现六页、TaskModel、TransferController、后台 BackupWorker、真实目录预览/备份、暂停取消/持久续传、ACK 进度、日志/设置 | 本轮 Windows 完整 CTest 22/22；真实 GUI→SSH 上传、kill 恢复见 M5 历史，本轮未重跑真实 GUI SSH |
| M6 当前文件列表 | 已实现 file_list_v1，服务端最多64条/64KiB，GUI每页32条、最近128游标、当前页筛选、错误保留旧页 | 本轮 gui_files_test 与 Linux files_list_test.py 通过；分页是实时视图，无跨页快照/总数保证；CLI 无列表命令 |
| M6 单文件下载 | 部分完成：服务端 download_v1 + Linux CLI，当前版本固定 FD，最多256KiB/块；临时文件、块/全文件校验、fsync/不覆盖发布 | 本轮协议和 Linux 真实下载集成通过；GUI/Windows CLI 下载、文件夹恢复、下载续传未实现 |
| 历史版本/目录快照 | 部分基础：schema v2 versions/version_chunks/current_files 和版本对象已存在；GUI VersionsPage 读取本机 WorkspaceStore 记录 | 服务端历史分页查询、任意旧版本恢复、目录快照均未实现。`--version-id` 只是当前版本前置条件，不能恢复旧版本 |
| 大文件/容量 | 未达到 R3：扫描器、SourceSnapshot、DeltaExchange、GUI 源读取、下载协议等存在64MiB限制，块数也有限制 | GB 文件和64MiB最大值完整端到端未在本轮验证；没有正式总容量/用户配额，不能只改一个常量 |
| 生命周期/运维 | 部分基础：事务、索引、过期会话处理、启动核验和 quarantine 已有；Logger.cpp 为互斥保护 stderr，GUI 有本机日志 | 无完整版本/快照保留回收和服务端日志轮转；隔离目录不是自动可删除垃圾。长期运行未验证 |
| 多用户/安全 | 未实现应用认证、身份命名空间、逐操作授权、管理员/用户配额、安全审计或 TLS 集成 | 当前仅 loopback/SSH；Qt runtime 带 TLS 插件不等于应用已经实现 TLS |
| 性能/规模 | 已有资源界限、工作池和网络功能测试；无有界多文件并行交付、FastCDC 实验或可复现性能报告 | Docker 弱网/压测及云公网验证未实现/未验证；不把100连接功能测试当性能指标 |

主要源码检查入口：`CMakeLists.txt`；`src/protocol/*`、`src/net/EventLoop.cpp`、`src/concurrency/DiskExecutor.cpp`；`src/storage/{UploadStorage.cpp,SqliteStore.cpp,schema_v2.sql,DeltaStorage.inc,DownloadStorage.inc}`；`src/client_core/*`；`apps/cli/DownloadClient.cpp`；GUI transport/controller/worker、MainWindow、VersionsPage；现有 unit 与 integration 测试。

## 本轮实际构建和测试

### Windows：Qt6.8.3 / MinGW13.1 / Ninja / Debug

在现有构建目录复用匹配 Kit 的缓存，没有新建/初始化源码工程：

```powershell
$env:PATH='D:/QT/Tools/mingw1310_64/bin;D:/QT/6.8.3/mingw_64/bin;'+$env:PATH
cmake -S . -B build-windows-gui
cmake --build build-windows-gui -j 2
ctest --test-dir build-windows-gui --output-on-failure
```

配置、构建均退出0，CTest **22/22通过**（56.84秒，仅测试用时）。配置为 SERVER/CLI/HASH=OFF、GUI/TESTS/DEPLOY_GUI=ON。测试包括协议/增量客户端/扫描器、gui_state、gui_files、backup_layout、gui_worker、gui_pages、gui_probe；Windows 此配置不覆盖 Linux 存储单测或 OpenSSL hash target。

非阻断告警：WrapVulkanHeaders 未找到，windeployqt 缺 dxcompiler.dll/dxil.dll；构建和测试仍通过，不等于正式安装包或 GPU 功能已验收。

### Ubuntu22.04 原工程：无 Qt 后端

实际经 SSH 执行（命令链任一步失败就停止）：

```bash
cd /home/ning/ChunkHarbor
cmake --build build --target chunkharbor-server chunkharbor-cli -j 2
python3 tests/integration/files_list_test.py ./build/chunkharbor-server ./build/chunkharbor-cli
python3 tests/integration/download_test.py ./build/chunkharbor-server ./build/chunkharbor-cli
```

整体退出0：server/CLI 构建成功；列表测试输出 committed-only paging/legacy/restart passed；下载测试输出 empty/binary/legacy/delta/version pinning/safe publication/restart passed。两脚本均使用 TemporaryDirectory 专用根和临时服务，不指向用户真实备份，不重启常驻服务。

本轮未重新执行 Linux configure（复用原缓存），也未重建先前按用户要求清理的全量测试可执行文件；`ctest -N` 注册50项只证明注册数量。Linux 全量50/50为2026-11-02历史结果，不能计为本轮通过。若未来运行全量 CTest，须先完整构建恢复测试产物，参考 UBUNTU_SYNC 的原依赖配置。

### 本轮未验证

Linux 全量 CTest、Release、ASan/UBSan/TSan、MSVC、Linux GUI、新的真实 Windows GUI→SSH 上传/恢复联调、最大文件与 GB 级端到端、物理磁盘满/硬件掉电、长期运行、性能/弱网/公网均未执行。本轮范围为文档审计和既有基线抽核，不扩展为这些验收；既有历史注入结果不当作真实硬件故障结果。

## 文档一致性与处理

| 差异 | 本轮处理 |
| --- | --- |
| AGENTS/指南仍要求按 M0–M7 推进，与 R1–R8 新排期冲突 | 局部增加新入口/原则，保留 M 定义和历史 |
| 指南将快照/版本回收列为可选，认证/性能集中在 M6/M7 | 明确 R2/R4/R5–R8 重排；不据旧优先级延后冻结项 |
| 指南的消息表、目录图、CLI push/pull 示例是早期建议，不能作为现行契约 | 加现行协议/源码入口提醒；现行命令仍以 README/专项文档/help 为准，不批量改写历史设计 |
| README 前文已写 GUI 真实上传，后文仍称“GUI原型不上传”，Windows章节仍按原型介绍 | 改为当前 GUI 和现行文档入口，补 R 路线图 |
| PROGRESS 中间/末尾仍有“GUI上传未实现”“下一步M5”，与顶部 M5/M6 记录冲突 | 顶部新增本次四类状态、测试证据及唯一下一项；用历史分界说明旧状态，不改写发生过的记录 |
| BUILD_WINDOWS 仍提同步后未提交，与后续发布记录冲突 | 当前说明指向最新同步和本次只读核验；专项历史验收的“未提交”等原文不改 |
| GUI 恢复按钮仍写“将在M6提供”，实际禁用；顶部连接徽标语义待核验 | 记录为 R1/R2 待办，本轮不修改产品代码/文字，不宣称已修复 |

## 风险、知识点与近期优先级

1. **R1 首项：连接状态语义与 GUI 回归。** `MainWindow.cpp` 的 statusChanged 总把徽标设离线，ready 设在线；QtTcpTransport 在探测握手后 terminal=true，而后续 disconnected 会更新状态；备份 worker 和文件列表又是独立连接。静态可见“检测结果/连接存活/传输状态”混合风险，但本轮没有针对用户症状建立运行复现，不能声称根因已确认。现有 gui_probe 检查握手退出码，不能充分证明 MainWindow 在后续状态变化时正确。
2. 下一轮先定义顶部显示为“最近检测结果/检测中/失败/待重查”等明确语义，增加握手后正常断开、服务端异常断开、端点切换、列表/上传独立结果、旧回调的窗口级测试，再最小修复。涉及 MainWindow/QtTcpTransport 和 GUI 测试；不改协议、数据库、上传工作线程所有权。验收既有22项与新增回归、相关真实SSH握手；是否改代码取决于复现结果。
3. R1 后续按已复现上传异常和模式矩阵逐个排；不能因旧重扫/布局修复存在而推断用户新症状已经解决，也不重复实现它们。R2 先复用 CLI 的当前单文件下载闭环，再加历史/快照语义。
4. 关键 C++/系统知识：RAII 保障进程内 FD/线程释放，不能替代崩溃恢复；TCP 已连接不等于 HELLO 成功或持久提交；不可变版本不等于目录快照；rename 原子可见不等于文件系统与 SQLite 跨存储事务；固定块增量不等于 CDC。

本轮文档已更新 PROGRESS；未提交/推送，未修改业务源码、协议、schema 或用户备份数据。
