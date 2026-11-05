# R3.1 GB 级大文件限制与资源审计

2026-11-04。依据 R2 标签、当前源码及 [R3 路线](ROADMAP_V3.md)。下文区分源码确认、既有验收记录和待验证方案。GiB、MiB、KiB 均使用二进制单位。

## 1. R2 基线

审计开始时分支 `codex/m5-gui`，HEAD 与标签 `r2-backup-restore-baseline` 均指向 `9f6078873790a13452fa6d4d77a7ba04ad616a10`。`git status --porcelain=v1 --untracked-files=all` 无输出。Git 忽略了 `build*/`、`cmake-build*/`、`tests/testFiles/`、Python 缓存和 IDE 设置。R2 Windows 日志与临时探针在 `build-windows-gui/r2-acceptance/` 及其父目录，外部 Ubuntu 测试根以验收文档为准。基线已建立。如需提交本轮成果，建议范围仅为本报告与 PROGRESS。

| R2 已关闭项 | 既有验收 |
| --- | --- |
| R2.1 当前单文件 | Windows GUI 下载当前 C，另核对空文件、中文路径、多块文件和当前版本前置条件冲突。 |
| R2.2 版本历史 | GUI 分页读取 C/B/A，旧版本迁移及当前标记正确。 |
| R2.3 历史单文件 | GUI 恢复 A，逐字节核对 A/B/C，覆盖真实 v2 夹具升级。 |
| R2.4 服务端快照 | B 快照在 C 发布及重启后保持同一映射，覆盖事务回滚、SIGKILL 和并发更新。 |
| R2.5.1 快照浏览 | 万项清单和 1,667 次分页，覆盖异常页、取消、断连与迟到响应。 |
| R2.5.2 旧快照目录恢复 | B 树及 129 文件串行恢复专项，覆盖目标竞争、短名别名及重解析点。 |
| R2.5.3 当前目录下载 | 固定创建时的 C，清单及数据读取时远端更新仍读固定版本。创建成功应答丢失后只查询候选。 |

既有 Windows 全量 31/32，原生 ComboBox 自动输入偶发失败。既有 Ubuntu 57/57，Windows 经 SSH 的同根闭环、上传持久 ACK 后双端中断续传、v2→v3→v4 迁移及旧会话续传通过。验收规模最大的闭环文件为 1,048,595 字节。限制包括 64 MiB 单文件、下载无续传、目录独立空目录和源端删除未记录、当前目录下载创建持久快照、无请求级幂等 ID、目录发布限 Windows 本机固定 NTFS。当前与历史单文件共用任务，目录任务独立且内部串行，两者可同时运行。同属主主动篡改、硬件掉电、真实盘满、MSVC、人工原生窗口及选择框、系统 DPI 切换或混合 DPI、远端万文件整树恢复未验收。可信本机或 SSH 隧道是部署边界。[完整验收与日志索引](R2_ACCEPTANCE.md)

本轮使用 `Get-FileHash tests/fixtures/*.tar.gz -Algorithm SHA256` 核对两个已跟踪夹具。v2 为 `66f39a57dc5d0d355de77dbc29f2717e82980384a09782ceda6ae0d31f205151`，v3 为 `75985bb33fca0fa915c90cf90d928844c4e12455694c5d5290ccb04786e24197`。

## 2. 限制清单与升级影响

位置以 R2 HEAD 行号为准。旧能力继续服务原尺寸和已持久化会话。表内「调整」均为后续任务。

| 当前限制 | 位置和作用 | 调整与兼容风险 |
| --- | --- | --- |
| GUI 单文件 64 MiB | [BackupPage 58](../apps/gui/pages/BackupPage.cpp#L58) 提示， [BackupWorker 223](../apps/gui/BackupWorker.cpp#L223) Windows 句柄与通用 QFile 两条源路径均拒绝，扫描/预览也会触发 | GB 入口必须同时更新源句柄校验和提示。保持相对 UTF-8、文件身份及源变化核对。 |
| CLI 单文件 64 MiB | [DirectoryScanner 45](../src/client_core/DirectoryScanner.cpp#L45) Linux 批扫描，[DeltaClient 182](../apps/cli/DeltaClient.cpp#L182)、[QtDeltaClient 118](../apps/cli/QtDeltaClient.cpp#L118) 增量源，[UploadClient 157](../apps/cli/UploadClient.cpp#L157)、[QtCliMain 194](../apps/cli/QtCliMain.cpp#L194) 旧 upload | 增量入口及其扫描器需按能力调整。旧 upload 保留旧值。扫描上界与远端可用能力分开，超限诊断在大规模哈希前给出。 |
| 共享增量 64 MiB | [SourceSnapshot 6](../src/client_core/SourceSnapshot.cpp#L6)、[DeltaExchange 32](../src/client_core/DeltaExchange.cpp#L32)、[ManifestPage 113](../src/protocol/ManifestPage.cpp#L113)、[PlanPage 45/78](../src/protocol/PlanPage.cpp#L45) 校验文件几何及 missing/reused 字节总量 | 必改增量路径的协商上限，保留旧能力语义。仅提高入口会导致清单或 PLAN_DONE 被拒绝。 |
| 服务端 64 MiB、`off_t` 最大值、65,536 块 | [UploadStorage 82](../src/storage/UploadStorage.cpp#L82) 的 `quotaGeometry` 被 [旧上传 begin](../src/storage/UploadStorage.cpp#L457) 和 [Delta begin](../src/storage/DeltaStorage.inc#L422) 共用 | 按能力分离文件上限，并在首次接受及重启恢复时核对同一几何。64 位文件 I/O 是先决条件。 |
| 旧 SQLite 会话 64 MiB 和块索引 0–65,535 | [schema_v1 6](../src/storage/schema_v1.sql#L6) 的 `transfers` / `transfer_chunks` CHECK 随 v2–v4 保留。[schema_v2 5](../src/storage/schema_v2.sql#L5) 的增量版本大小为 INTEGER，会话元数据保存在 request JSON | 增量路径可容纳目标大小，暂无需迁移。旧 upload 保持原 CHECK。若以后扩展旧路径，必须单独迁移并验证外键、版本对象、旧会话和快照。 |
| 上传会话共享 32 个、声明大小总计 512 MiB | [UploadStorage 338](../src/storage/UploadStorage.cpp#L338) 旧会话启动重建硬检查，[484](../src/storage/UploadStorage.cpp#L484) 旧入口会计入 Delta，[DeltaStorage 432](../src/storage/DeltaStorage.inc#L432) Delta 反向计入旧路径，上传 phase < 5 计入 | GB 启用前必须建立独立于单文件限制的总预算，保留会话数量防护。现状累计声明大小，未涵盖对象复制、DB/WAL、保留版本或其他写入者。降配重启、重复 BEGIN/COMMIT、失败清理及并发预留均需验收。 |
| 预览独立配额 | [DeltaStorage 149](../src/storage/DeltaStorage.inc#L149)、[446](../src/storage/DeltaStorage.inc#L446)、[486](../src/storage/DeltaStorage.inc#L486)、[520](../src/storage/DeltaStorage.inc#L520)。收集中的预览 8、预览记录 20,000、估算元数据总计 64 MiB。页正文、哈希与 range 行分别计账 | 正文增大无需同比放宽。保持有界预览，用实测校准会计，测试配额耗尽和回收。 |
| 65,536 块、每页 512 哈希、至多 128 页 | [ManifestPage 10](../src/protocol/ManifestPage.cpp#L10)、[DeltaControl 90](../src/protocol/DeltaControl.cpp#L90)、[SourceSnapshot 9](../src/client_core/SourceSnapshot.cpp#L9)、[DeltaExchange 32](../src/client_core/DeltaExchange.cpp#L32)、[DeltaPlanner 19](../src/client_core/DeltaPlanner.cpp#L19)、[QtDeltaClient 152](../apps/cli/QtDeltaClient.cpp#L152)、[UploadClient 166](../apps/cli/UploadClient.cpp#L166)、[BackupWorker 75](../apps/gui/BackupWorker.cpp#L75) | 建议保留块数上界。按实际文件尺寸校验块大小下界。升块数会连带修改清单分页、恢复位图、范围和资源预算。 |
| 计划每页 512 range、64 页，终止查询索引可为 64 | [PlanPage 5](../src/protocol/PlanPage.cpp#L5)、[DeltaControl 136](../src/protocol/DeltaControl.cpp#L136)、[DeltaExchange 90](../src/client_core/DeltaExchange.cpp#L90)、[DeltaStorage 597](../src/storage/DeltaStorage.inc#L597)。交替缺失最多 32,768 个 range。旧 UPLOAD_PLAN 单帧最多 512 range，[UploadPlan.h 6](../include/chunkharbor/protocol/UploadPlan.h#L6) | 保留固定几何与分页上限，测试交替缺失和终止页。旧方案的续传缺块走 SESSION_STATUS。 |
| 恢复位图 8 KiB 原始、16 KiB 十六进制 | [SessionStatus 23](../src/protocol/SessionStatus.cpp#L23)、[DeltaControl 178](../src/protocol/DeltaControl.cpp#L178)、[DeltaStorage 734](../src/storage/DeltaStorage.inc#L734) | 保持位图格式与尾部填充检查。检查点记录不是位图，保留旧记录可读。 |
| 上传帧和分块上界 | [FrameCodec.h 8](../include/chunkharbor/protocol/FrameCodec.h#L8) 帧头 32 B、控制 64 KiB、数据 1 MiB。[DataChunk.h 7](../include/chunkharbor/protocol/DataChunk.h#L7) 块头 60 B，块最大 1,048,516 B。[SettingsPage 44](../apps/gui/pages/SettingsPage.cpp#L44) 64/256/512 KiB，默认 256 KiB，[DeltaCommand 97](../apps/cli/DeltaCommand.cpp#L97) 默认 256 KiB | 保留帧上界和块语义。新会话可按大小选择受支持几何。旧会话/基线按原块大小匹配，变化会影响复用率。 |
| 当前及历史下载 64 MiB、块 256 KiB | [Download.h 7](../include/chunkharbor/protocol/Download.h#L7)、[Download.cpp 14](../src/protocol/Download.cpp#L14) READY 大小、请求范围和 DATA 块长度校验，[DownloadStorage 32/55](../src/storage/DownloadStorage.inc#L32)、[DownloadController 111](../apps/gui/DownloadController.cpp#L111)。[Linux DownloadClient 198](../apps/cli/DownloadClient.cpp#L198) 当前下载复用协议校验 | 两种选择方式都需按能力处理 GB，旧当前版本冲突及历史 path+ID 绑定必须保留。保留下载帧块上界。历史与快照的主入口是 GUI。 |
| 快照 64 MiB 每项、10,000 项；列表 64 条、清单每页 6 条 | [SqliteSnapshot 27](../src/storage/SqliteSnapshot.inc#L27) capture 超限会使引用约束失败并回滚整个快照，[DirectorySnapshot 125](../src/protocol/DirectorySnapshot.cpp#L125) 编解码，[DirectorySnapshot.h 12](../include/chunkharbor/protocol/DirectorySnapshot.h#L12)、[SnapshotRestore 43](../apps/gui/SnapshotRestore.cpp#L43) 和 [schema_v4 8](../src/storage/schema_v4.sql#L8) | 调整单项限制须同时覆盖快照创建、查询、当前目录与旧快照恢复。旧客户端请求含大文件快照时返回明确错误，不能静默漏项。保持计数和分页，另加目录本地容量预算。文件列表和历史每页各 64，大小字段可表达 GB，旧客户端可能仍列出无法下载的版本。 |
| 批任务及路径元数据 | [DirectoryScanner 45](../src/client_core/DirectoryScanner.cpp#L45) 10,000 文件，[BackupWorker 353/386/425](../apps/gui/BackupWorker.cpp#L353) 10,000 路径及 20,001 检查点记录，[DeltaCommand 222](../apps/cli/DeltaCommand.cpp#L222) 批状态记录，[BackupWorker 106](../apps/gui/BackupWorker.cpp#L106)、[DeltaCommand 49](../apps/cli/DeltaCommand.cpp#L49) 单记录 16 KiB。[BeginUpload.h 5](../include/chunkharbor/protocol/BeginUpload.h#L5) 逻辑路径 4,096 B、组件 255 B | 保持既有边界。目录总数据可达 TB 级，文件数与清单页均不能代替字节预算。 |
| HELLO 能力只协商功能和帧上限 | [Handshake.cpp 44/78](../src/protocol/Handshake.cpp#L44) 最多 16 个 token，HELLO payload 4 KiB，严格键集合。[UploadHandler 64](../apps/server/UploadHandler.cpp#L64) 仅确认客户端提出的 upload、fixed_block_delta_v1、download_v1、historical_download_v1、directory_snapshot_v1 等功能 | 上线 GB 前须明示双方支持的几何和操作；不能向 v1 JSON 直接加未知字段或对未提出的能力强行应答。建议显式能力加独立限额查询，冻结既有能力的行为。磁盘可用额度需在 BEGIN 时重新核对。 |
| 服务器连接/工作池/缓冲 | [server main 27/78/92](../apps/server/main.cpp#L27) upload 默认 32、最多 64 连接，2 worker、64 在途任务，含待完成回调。[TcpConnection.h 13](../include/chunkharbor/net/TcpConnection.h#L13) 输入 1 MiB + 32 B、输出 4 MiB、高低水位 1 MiB/512 KiB | 保留硬边界和背压。以连接、任务和在途帧测峰值，避免把 512 KiB 低水位混为磁盘配额。 |
| 配置与测试 | [server.example.json](../configs/server.example.json) 只示例 host/port/root，当前 server main 实际解析命令行，未读取该 JSON。测试边界见 [manifest_page_test 119](../tests/unit/manifest_page_test.cpp#L119)、[delta_control_test 47](../tests/unit/delta_control_test.cpp#L47)、[delta_planner_test 22](../tests/unit/delta_planner_test.cpp#L22)、[session_status_test 12](../tests/unit/session_status_test.cpp#L12)、[download_test 21](../tests/unit/download_test.cpp#L21)、[upload_storage_test 165/270](../tests/unit/upload_storage_test.cpp#L165)、[upload_test 77](../tests/integration/upload_test.py#L77) | 保留旧负向边界测试，新增能力专项与配置单位/解析上界测试。64 KiB 测试帧和 SHA-256 I/O 缓冲不随文件上限改动。 |

## 3. 类型、内存与资源所有权

- **文件正文**。上传源以偏移读一个块，整文件 SHA-256 在 Windows GUI/Qt CLI/Linux CLI 及服务端使用增量更新。服务端 `hashFd`、`copyFd` 使用 64 KiB 缓冲。客户端下载逐请求读至多 256 KiB，检查块摘要、写暂存并更新整文件摘要后才发布。未在主传输路径发现整文件正文读入内存。下载和快照进度事件会合并，备份常规进度按 100 ms 节流。多个编码副本、socket 缓冲和 OS 页缓存仍应分别测量。[SHA-256 实现](../src/common/Sha256.cpp#L51) · [DeltaExchange](../src/client_core/DeltaExchange.cpp#L150) · [DownloadController](../apps/gui/DownloadController.cpp#L130) · [Linux 下载](../apps/cli/DownloadClient.cpp#L198)
- **Socket 例外**。Qt GUI 上传、下载及 Qt Delta CLI 显式限制 socket 接收缓冲。Windows 旧 upload CLI 的 `readAll()` 读 socket，未配置同级接收上界，虽然解析会拒绝超限帧，仍应在兼容回归纳入畸形对端测试。GUI 上传的文件 `readAll()` 用于事先限制到 16 KiB 的检查点。[Qt 旧 CLI](../apps/cli/QtCliMain.cpp#L50) · [GUI 检查点](../apps/gui/BackupWorker.cpp#L104)
- **文件大小、偏移及累计字节**。协议大小和偏移为 `uint64_t`，JSON 协议解析使用整数，Qt 持久状态的大整数使用字符串。Linux `off_t`、Qt `qint64`、SQLite `INTEGER`/bind_int64 可支撑目标规模的前提是构建与转换验证。Linux 要求 64 位 `off_t`，Windows 保持 64 位文件 seek；验证 DB 非负后再转无符号。范围用 `offset <= size && length <= size - offset`，块数用商加余数，累加/乘法先做上界检查。DWORD 和 size_t 仅用于经验证的单块或内存长度。[DataChunk](../src/protocol/DataChunk.cpp#L59) · [SqliteStore](../src/storage/SqliteStore.cpp#L181) · [SnapshotOutput](../apps/gui/SnapshotOutput.cpp#L237)
- **明确的 32 位显示缺陷**。`SnapshotPage` 将文件大小和已传字节直接转为 `int` 作为 QProgressBar 范围和值，从 2 GiB 起不能正确表达。应使用固定进度刻度及独立 64 位字节文本，涵盖空文件、完成和取消。备份及单文件页用 64 位值算百分比，在当前建议范围内可表示，继续防止聚合乘法溢出。块索引、页号、每块长度在 65,536 块契约内可保留 32 位，乘块大小前必须提升为 64 位。[SnapshotPage](../apps/gui/pages/SnapshotPage.cpp#L752) · [BackupPage](../apps/gui/pages/BackupPage.cpp#L185) · [FilesPage](../apps/gui/pages/FilesPage.cpp#L214)
- **元数据**。客户端每次生成 512 哈希的页，32 B 摘要原始约 16 KiB，缺失 `vector<bool>` 约 8 KiB。满 65,536 块的旧上传会话另有约 2 MiB 摘要向量，最多 32 个会话的理论摘要量约 64 MiB。增量每块十六进制摘要一份即约 4 MiB，页 JSON、块行、版本索引、计划和 WAL 会进一步放大磁盘用量。Delta manifest 重连会从头重新生成页并跳过已确认页，不能把网络少传等同于本地少读。目录 GUI 常驻完整清单、预检和树节点，内存与路径数量和长度有关。[ManifestReader](../src/client_core/SourceSnapshot.cpp#L11) · [DeltaExchange](../src/client_core/DeltaExchange.cpp#L70) · [旧会话向量](../src/storage/UploadStorage.cpp#L121) · [增量表](../src/storage/schema_v2.sql#L9)
- **目录恢复**。每文件独立下载连接，逐文件完成摘要及刷盘，网络与数据缓冲及时释放。Windows `flushFile` 只清除当前裸句柄，`Node::file` 持有句柄至发布前 `closeFiles` 或失败清理，故句柄随文件数增长。须测句柄/清理延迟，初期可限制单任务资源并及早拒绝。若要逐文件释放句柄，须有基于父句柄的受控重开及身份、链接数和发布竞态校验设计，保留无覆盖、无重解析点及外来对象保护。[SnapshotRestore](../apps/gui/SnapshotRestore.cpp#L87) · [SnapshotOutput](../apps/gui/SnapshotOutput.cpp#L229)

## 4. 长操作、持久性与阻塞风险

| 优先级 | 源码证据与后果 | 阶段门槛 |
| --- | --- | --- |
| P0 共享锁覆盖整文件 I/O | `manifestDone` 在共享存储/DB 锁和 BEGIN IMMEDIATE 内读哈希基线，`getPlan` 最终页复制复用块并刷盘，`query` 重读已确认块，`commit` 整文件哈希及生成完整对象，均持共享锁。2 个 worker 中另一个可卡在同一锁，影响其他连接，即使 Reactor 仍在派发。下载首帧前 `openDownload` 在 DB 锁外完整预哈希，也会长期占 worker。 | GB 启用前需验证任务隔离。以固定对象 FD、路径/会话租约和短事务划清所有权。分段长操作时重核基线和状态后提交，崩溃可回到可重试状态。保留原子发布、完整校验及 ACK 顺序。[DeltaStorage 507](../src/storage/DeltaStorage.inc#L507) · [getPlan 593](../src/storage/DeltaStorage.inc#L593) · [query/commit 720](../src/storage/DeltaStorage.inc#L720) · [DownloadStorage 49](../src/storage/DownloadStorage.inc#L49) |
| P0 空间与发布写放大 | 增量上传 `ftruncate` 暂存可以是稀疏的。改一块也要物化复用块，版本对象是全量复制，暂存再发布为当前文件。若旧当前和其历史对象都存在，发布前可能同时占用旧当前、旧对象、新暂存和新对象约 4S，另计更老版本及元数据。ENOSPC 可发生在写、复制、同步或提交阶段。当前未发现按文件系统剩余空间做传输准入的调用。Windows 目录须预留整树且可能同时单文件下载。 | 服务端按卷、实际已用和在途承诺建立原子预留，基于实际可用空间保留安全余量，预检之后仍处理实际 ENOSPC/EDQUOT。峰值按阶段计费避免重复或漏算，释放只发生在确认清理/发布后，启动重建。未有长期回收前按保留版本设置有限测试预算。[DeltaStorage 63/275/350](../src/storage/DeltaStorage.inc#L275) · [UploadStorage 28](../src/storage/UploadStorage.cpp#L28) |
| P0 请求等待、取消与结果歧义 | GUI 上传 connect 5 s、写和等响应各约 30 s。Qt CLI connect 5 s，写/读等待 30 s，逐次等待可延长总时限。Linux 上传/Delta/下载 connect 5 s，单次交换 30 s。GUI 下载 connect 5 s、写与响应各 10 s。Qt 列表/健康传输 connect 5 s、应答 10 s，健康周期 15 s。服务端默认空闲 30 s（CLI 最大 3,600,000 ms），磁盘工作暂停读取期间豁免空闲回收，客户端超时仍独立。工作闭包继续执行，弱连接回调可丢弃迟到结果。长本地哈希/暂停也可能让非忙服务端连接空闲断开。 | 区分 connect、字节无进展、队列等待、存储阶段进展及阶段总预算。建议协商有界长操作状态，允许独立查询已持久阶段；保留明确超时和退避。断连或取消不能推断提交失败，查询已提交结果；安全点前可取消，持久发布后返回既有结果。避免靠无限超时或移除校验。[BackupWorker 120](../apps/gui/BackupWorker.cpp#L120) · [DownloadController 26](../apps/gui/DownloadController.cpp#L26) · [TcpServer 61](../src/net/TcpServer.cpp#L61) · [UploadHandler 146](../apps/server/UploadHandler.cpp#L146) |
| P0 重启与持久 ACK | Delta 构造时检查并修复提交、对象与接受记录；查询也重验已接受块，恢复成本可能接近读完整文件。预览按创建时刻 1 天，上传 30 天，过期检查见启动/BEGIN/加载路径。旧上传启动重建 receipt 与 SQLite 状态。逐块数据同步后才写持久记录并 ACK，SQLite WAL + FULL。服务停止会 join 并处理队列，耗时取决于在途任务。 | 用故障点验证同步前后、ACK 前后、对象复制、发布 rename 与 DB 更新之间的重启。区分启动扫描、按需查询与后台修复，按字节或时间测恢复成本；控制同一会话重复恢复。保留旧夹具、同 ID 幂等和无覆盖。[DeltaStorage 98/191](../src/storage/DeltaStorage.inc#L98) · [UploadStorage 595](../src/storage/UploadStorage.cpp#L595) · [SqliteStore 72](../src/storage/SqliteStore.cpp#L72) · [DiskExecutor 59](../src/concurrency/DiskExecutor.cpp#L59) |
| P1 GUI 响应性与本地安全 | GUI worker 持有 socket；取消在分块、轮询或阶段检查点生效，阻塞文件 I/O/刷盘不能即时打断。关窗走取消与收尾，析构或回收等待 worker。快照发布前 CAS 提交点，之后取消不可撤销发布。下载失败清理暂存，目前无持久续传。 | 测量取消请求至 worker 退出、关窗响应和暂存释放。R3 建议延续下载失败清理、重试从头且固定历史 ID，明确 UI 文案。未来续传需要单独定义暂存身份、已持久范围和重新校验。目录句柄预算与 2 GiB 进度修复纳入对应阶段。[MainWindow 311](../apps/gui/MainWindow.cpp#L311) · [SnapshotRestore 113](../apps/gui/SnapshotRestore.cpp#L113) · [DownloadController 155](../apps/gui/DownloadController.cpp#L155) |
| P2 固定块吞吐 | 一次一请求，上传每块 sync+SQLite，下载 256 KiB 每块往返。高 RTT 下近似上界为块大小/RTT，另有同步、磁盘、校验开销。少发网络块仍可能多次全量读写。 | R3.4 分离计时并做对照，在实测瓶颈上优化。固定分块保持，FastCDC 留 R7。 |

## 5. 分阶段升级方案

以下是建议，文件上限与配额都以可复现验收为启用门槛。保持 65,536 块时最小固定块大小是 `ceil(file_bytes / 65536)`，因此只需控制几何即可到 10 GiB。128 个 manifest 页及最坏 64 个 plan 页均可保留。

| 文件 | 64 KiB 块数 | 256 KiB 块数 | 512 KiB 块数 | 最小块 |
| --- | ---: | ---: | ---: | ---: |
| 1 GiB | 16,384 | 4,096 | 2,048 | 16 KiB |
| 4 GiB | 65,536 | 16,384 | 8,192 | 64 KiB |
| 10 GiB | 163,840，超限 | 40,960 | 20,480 | 160 KiB |

### R3.2 1 GiB

1. **最小先行任务**。在 `DeltaStorage::manifestDone` 上做长规划与短事务隔离，保持现有尺寸、协议和 schema。将基线读取/哈希移出共享 DB 锁和长写事务。用不可变基线 FD、同会话排他与有界分页暂存计划，事务内重验阶段、会话和基线后安装计划。以慢基线读取注入，验证独立列表与小文件下载可进展、同会话重复 DONE 结果一致、基线竞争按原语义拒绝或仍固定基线、SIGKILL 后无半成品计划。验收以现有尺寸完成，属于 R3.2 首个子任务。
2. **其余启用门槛**。依次解决 getPlan 复用物化、query/启动重验、commit 与下载预哈希的长操作隔离和有界进展，建立容量/在途预留及可测试盘满行为，再联通协议能力、共享几何、GUI/CLI、当前/历史与快照下载。优先验证 256 KiB 块，兼测旧 64 KiB。保留旧 upload/v1 兼容路径。下载取消清理后从头重试，上传从已持久范围续传。
3. **阶段验收**。实际非稀疏 1 GiB 完成上传、增量、续传、当前和历史下载、旧及当前快照恢复，所有摘要及版本映射一致。验证上限加一字节拒绝、v2/v3 夹具及旧客户端、真实容量不足、提交应答丢失后查询一致，以及慢操作时其他请求仍可服务。容量门槛由并发预留、真实空间和下表采样确定。

### R3.3 4 GiB 与 10 GiB

先单独通过 4 GiB，再验证 10 GiB。复用 R3.2 的几何协商、容量会计、任务状态与持久契约。涉及 Linux 文件 I/O 检查、GUI 进度、目录句柄和总暂存预算、SQLite/JSON 边界与测试工具。补测 2 GiB、4 GiB 前后各一字节及跨边界的真实块读写。4 GiB 用 64 KiB 命中 65,536 块，测试最后索引、128 manifest 页和 64 plan 页；10 GiB 接受 256/512 KiB，拒绝 64 KiB 的组合。避免静默改块大小导致原增量基线失配，选择新几何时给出重新生成计划的提示。每档均运行重启、容量、恢复、取消及版本钉住测试。只有双端目标文件系统和实际非稀疏样本通过后才启用该档。

### R3.4 性能测量与优化

先记录首次、无变化、一块变化、交替缺块、上传续传、当前/历史/快照恢复的耗时和 I/O，分离扫描摘要、manifest、基线比较、复用复制、ACK、整文件校验、发布、首字节和持续传输。控制块大小、RTT、缓存冷热及磁盘条件，按实测热点逐项尝试减少重复读取或元数据写入、批次持久化或有限在途窗口。批次确认仍须在该批数据及记录持久后发送。每项单独报告对照、峰值资源和故障回归，保持固定分块。

## 6. 可靠性与性能验收矩阵

样本使用测试专用 NTFS 路径和 Ubuntu 临时卷/临时根，SSH 只转发回环，拒绝真实备份根。用固定种子分段写入不可压缩数据，记录逻辑大小、分配量及 SHA-256；稀疏样本仅用于寻址边界。以 0、块长前后一字节、64 MiB 边界以及各档上限前后为基础，每档至少一个实际满尺寸样本。开始前记录卷预算与工具链。仅清理脚本创建且已确认归属的测试根。

| 场景 | 注入与断言 |
| --- | --- |
| 全新上传 | GUI 和 Linux/Qt CLI 增量入口，断言 ACK 块数、文件摘要、版本及当前映射。旧 upload 仍覆盖 64 MiB 边界。 |
| 增量和预览 | 无变化传零 DATA_CHUNK，改一块只发送该块，交替缺块覆盖最坏范围。改末尾短块、源文件变化、换块几何、基线发布竞争。核对预览和实际计划、旧版本不变，记录实际读写。 |
| 上传中断、续传 | 清单中段、数据同步前后、ACK 后、全部持久但未提交、提交完成应答丢失。杀客户端并重启，检查同一会话、真实缺失位图、无重复版本，损坏已确认块应被发现。 |
| 服务端重启与慢操作 | 在复用复制、提交预校验、对象同步、rename 和 DB 状态协调的各窗口 SIGKILL。重启核对 DB integrity/外键、配额、当前/历史内容及残留。注入超时内慢 I/O、阶段无进展及慢对端，同时请求另一小文件/列表，验证有界排队和超时恢复。 |
| 当前及历史单文件下载 | A/B/C 至少含大文件，读 A/C 后逐字节摘要核对。下载期间发布 D，固定版本结果正确，旧当前条件返回冲突，错误 path+ID 被拒绝。 |
| 快照恢复 | 旧 B 和当前 C，各含一个大文件、多个小文件、中文路径及空文件。完整树对比、并发发布、目标冲突及预检盘不足；混合 129/万项元数据回归，记录句柄峰值。 |
| 下载断连、取消及关窗 | 在 READY 前、传输中、刷盘/发布边界分别注入。保留现有目标，未发布结果不出现，暂存可安全清理或报告残留。重试整文件且历史仍使用固定 ID。测取消至释放时间。 |
| 校验错误及磁盘不足 | 注入坏块、整文件错、损坏历史对象、同步错误；再用独立小卷真实触发 ENOSPC/EDQUOT，分别覆盖接收、复用/版本复制、SQLite WAL 和 Windows 恢复输出。核对无虚假 ACK、旧版本可读、重启预留正确。注入结果与真实盘满分列。 |
| 兼容性与资源耗尽 | 新旧双向组合、旧 v2/v3 夹具、过期会话、配额/连接/队列满、畸形几何和大清单。容量超限在写正文前给出可解释拒绝；连接退出后 FD/队列回落。 |

采样包含 Windows GUI/CLI 工作集、私有字节、CPU、句柄和磁盘写入，Linux 服务端 RSS/峰值、CPU、FD、每阶段读写、WAL 和暂存实际分配、网络字节、队列深度与等待时间。可用 `/proc/<pid>/status`、`/proc/<pid>/io`、`/proc/<pid>/fd`、`du` 与 Windows 性能计数器。记录首字节、总耗时、有效吞吐 MiB/s、持久 ACK 及短请求 p50/p95，以及取消/重启收敛时间。每条件至少三次，记录 OS/FS、版本、SSH/RTT、块大小、冷热缓存、有效容量和异常注入点。验收要求在同一连接数/块大小下从 1 到 10 GiB 的进程缓冲峰值不按正文线性增长，单个慢任务不能使其他任务无界等待，资源峰值与清理残留均有记录。具体吞吐和时延目标由 R3.2 首轮测量确定。

## 7. 本轮验证边界

本轮只完成源码审计与升级设计。仓库状态、标签、夹具摘要、源码入口及限制交叉核对属于本轮证据。Windows 11 的 Qt 6.8.3 / MinGW 13.1 配置、增量构建及 offscreen CTest 31/31 通过。该构建启用 GUI 与 tests，CLI、服务端及 hash 选项关闭。命令、非阻断构建提示和排除的原生专项见 [进度记录](PROGRESS.md)。本轮未执行 Linux 构建、SSH 端到端、GB 传输、容量耗尽或性能采样，表中的 GB 方案和风险均待后续验证。
