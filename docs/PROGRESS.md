# ChunkHarbor 开发进度

## R2 Git 基线

- 阶段版本使用标签 `r2-backup-restore-baseline`，保存 R2 实现、测试和验收记录。迁移测试仅纳入 [v2 与 v3 合成夹具](../tests/fixtures/README.md)。本地构建产物、验收日志及临时备份留在 Git 之外。

## R2 最终综合验收（2026-11-04）

- **达到关闭标准。** Windows 在同一隔离 Ubuntu 根上经 SSH 完成 auto A/B/C、服务端重启、当前单文件 C、历史单文件 A、旧快照 B 和当前目录 C。Windows 输出逐文件匹配原内容及 SHA-256。恢复前后当前指针、六个不可变版本对象和历史版本行一致。
- 真实 SSH 回归覆盖 R1 持久 ACK 后双端进程中断续传、v2→v3→v4 后恢复 A/B/C、清单和首个数据块读取时远端继续更新。另在服务端提交后实际丢弃创建应答，GUI 只重连查询候选，没有重复创建。
- Windows 配置与增量构建成功，完整 CTest **31/32**，103.12 秒。唯一失败是 R1 已记录的原生 ComboBox 自动输入问题。独立原生专项实际 DPR 1.0、1.25 通过，1.5 在最大化设置页复现失败。人工鼠标和系统 DPI 切换未验收。Ubuntu 当前源码隔离构建与完整 CTest **57/57**，41.61 秒。
- 64 MiB、下载无续传、独立空目录和源端删除标记、持久快照、请求级幂等缺口、Windows NTFS 边界及未验证风险已分类记录。完整日志、实际 SHA-256 和 R3 前基线见 [R2 验收](R2_ACCEPTANCE.md)。未改生产代码或测试配置，未提交或推送。R3 等待另行指令。

## R2.5.3 Windows 当前文件夹下载（2026-11-04）

- 「目录快照」页增加「下载文件夹」。经持久快照确认后，查询同根最新序号作为基线，发送一次创建，按返回 ID 核对完整清单。零文件显示无已提交文件，已创建的空快照保留，本地不发布目录。
- 复用快照预检、`SnapshotRestore` 串行历史版本下载、分块与整文件 SHA-256、`SnapshotOutput` 的 NTFS 暂存及无覆盖发布。服务端协议、schema 与 64 MiB 单文件限制保持原有语义。
- 创建应答不确定时只读查询原端点。新增记录仅作为候选，需手动核对。关窗和切换端点会取消尚未发出创建请求的流程。已发出创建请求时，关窗会等待确认或核对。
- Windows Qt 6.8.3 / MinGW 13.1 构建退出 0，`ctest --test-dir build-windows-gui -LE native-desktop --output-on-failure -j 2` **31/31**，106.28 秒。
- Ubuntu 隔离构建 CTest **57/57**。真实 SSH 联调在清单读取和数据块读取时分别提交了新版本，Windows 仍恢复快照原来的 3 个文件，逐文件 SHA-256 匹配，且未跨逻辑前缀。详见 [R2.5.3 交付](R2_5_3_CURRENT_FOLDER_DOWNLOAD.md)。
- 丢应答故障使用本机可控对端。真实 SSH 丢应答、强制退出、掉电、MSVC、原生目录选择框和远端万文件完整下载未验证。本轮停在 R2.5.3，R2 综合验收等待后续指令。

## R2.5.2 Windows 指定快照整目录恢复（2026-11-04）

- 「目录快照」页可选择已有父目录和新目录名。完整清单及预检有效时允许确认恢复，执行固定快照 ID 和不可变版本映射。串行复用 R2.3 下载与 SHA-256 校验，进度包含已刷盘文件数及字节数。
- Windows 固定 NTFS 盘上按父句柄独占创建暂存树，拒绝穿越、路径别名和重解析点。核对暂存成员后以同卷一次重命名发布，内核拒绝覆盖竞争目标。只清理本任务对象，保留并报告外来成员或清理失败。服务端协议、存储和当前指针无需变更。
- Windows Qt 6.8.3 / MinGW 13.1 构建退出 0，`ctest --test-dir build-windows-gui -LE native-desktop --output-on-failure -j 2` **31/31**，92.63 秒。恢复专项覆盖 129 文件的串行恢复、万项清单分页、失败清理、目标竞争和关窗。
- Ubuntu 隔离构建 CTest **57/57**，42.14 秒。真实 Windows→SSH→Ubuntu 分别恢复同一路径的两个快照，新版 `DDDD` 与旧版 C、旧上传文件内容和 SHA-256 一致。见 [R2.5.2 交付](R2_5_2_SNAPSHOT_RESTORE.md)。
- 真实盘满、突然掉电、MSVC、原生目录选择框及远端万文件整目录恢复未验证。选定父目录在执行时固定，暂存 ACL 隔离其他账号。同属主恶意改动和跨操作的掉电持久性超出本轮验证。下一子任务为 R2.5.3 当前目录下载。

## R2.5.1 Windows GUI 目录快照浏览与恢复预览（2026-11-04）

- 新增「目录快照」页。按逻辑根分页列出快照，核对选中快照头，逐页拉取固定 `snapshot_id` 的完整清单，展示相对路径、版本 ID、大小、SHA-256、文件数及总字节数。本地展示页最多 100 行。取消、切换、断连或响应不一致会清除未完成的清单。
- 目标路径预检在后台线程执行。报告 Windows 非法名称、路径长度、大小写及文件和目录冲突、已有目标、被占用的父目录和重解析点。预览只读取本地属性。实际恢复仍须重新检查短名别名、路径竞态和发布安全。
- 沿用 `directory_snapshot_v1`、64 KiB 帧和六条清单页。Windows Qt 测试读完 10,000 项、1,667 次清单请求。真实 Windows→SSH→Ubuntu 在快照创建后提交了新版本，Qt 仍读取 2 条旧映射及原 SHA-256。Ubuntu 在隔离的 R2.4 构建上重新运行 CTest **57/57**。
- Windows Qt 6.8.3 / MinGW 13.1 下 `cmake --build build-windows-gui -j 2` 退出 0，`ctest --test-dir build-windows-gui -LE native-desktop --output-on-failure -j 2` **31/31**，93.96 秒。
- 快照是服务器已提交文件版本集合。独立空目录和源端删除标记未入库。原生目录选择、原生桌面下拉框专项、MSVC、远端 10,000 项延迟及预检系统调用挂起时的退出延迟未验证。详见 [R2.5.1 交付](R2_5_1_SNAPSHOT_PREVIEW.md)。下一子任务为 R2.5.2 恢复执行。

## R2.4 目录快照创建、持久化与查询（2026-11-04）

- 服务端显式捕获指定逻辑根下的一致已提交文件版本集合。兼容 `current_files` 和未整理的已提交旧上传。允许零文件清单。空目录和不存在的前缀没有独立记录。源端删除、改名和部分成功按现有服务器状态体现。
- schema v4 新增快照头、相对路径条目、路径/序号索引和历史及对象元数据外键。单事务从创建态写入映射并发布，失败或提交前退出不留下可查询的快照。不复制文件内容。单快照最多 10,000 文件。
- HELLO 新增 `directory_snapshot_v1`，帧 37–43 支持创建、列表、元数据及条目分页。列表最多 64 条，条目最多 6 条，控制帧 64 KiB。共享协议及测试探针提供最小客户端入口。
- v3 真实归档副本验证迁移 SQL 回滚、提交前退出、重启、版本及检查点保留。事务测试覆盖插入中途失败、SIGKILL、并发备份和第二个写事务被拒绝。还覆盖 70 条真实上传分页、10,001 条上限拒绝、无效引用、外键保护、空清单与源端删除。
- Ubuntu 配置与构建退出 0，`ctest --test-dir build --output-on-failure` **57/57**，41.27 秒。补充锁断言后故障专项 1/1。Windows Qt 6.8.3 / MinGW 13.1 配置与构建退出 0，`ctest --test-dir build-windows-gui -LE native-desktop --output-on-failure` **30/30**，145.71 秒。首轮 Linux 54/55 为列表字段断言错误，已纠正并保留日志。
- Windows 经真实 SSH 创建快照并核对元数据、列表和两条清单摘要。Windows Qt 从 v4 服务端恢复 A/B/C，以及下载当前空、中文和多块文件，大小及 SHA-256 均一致。R1–R2.3 相关回归通过。全部联调使用隔离源码树和备份根。
- 提交后应答丢失、掉电、原生下拉框、MSVC 和真实库升级未验证。提交成功但应答丢失时可查列表，重发创建可能产生另一个完整快照。实际命令和变更清单见 [R2.4 交付](R2_4_DIRECTORY_SNAPSHOT.md)。未提交或推送，本轮停在 R2.4。

## R2.3 指定历史版本下载与安全恢复（2026-11-04）

- Windows 版本历史页可选中服务器版本并下载到指定本地路径，复用窗口级 DownloadController 和安全发布流程。进度栏显示完整版本 ID。换文件、换版本或离开历史页时，任务继续使用开始前固定的元数据。
- 新协商能力 `historical_download_v1` 使用起始帧 36，必须同时提供逻辑路径及版本 ID。服务端按发布记录绑定两者，打开 `.versions/<id>/data` 并固定 FD，核对大小及 SHA-256 后沿用 32/33/14 下载帧。历史读取不会改写当前指针。现有 `download_v1` 仍要求匹配当前版本。schema v3 沿用。
- 正常传统上传及启动修复先持久化不可变对象，再登记 COMMITTED。测试在副本刷盘前暂停，验证历史尚不可见。此时终止进程，重启可补齐对象并恢复内容。复核也修正了历史页在有记录时可能误报空页的问题。
- Windows Qt 6.8.3 / MinGW 13.1 配置与构建退出 0，`ctest --test-dir build-windows-gui -LE native-desktop --output-on-failure` **29/29**，143.91 秒。Ubuntu GCC 11.4 在隔离源码树配置与构建退出 0，`ctest --test-dir build --output-on-failure` **54/54**，41.63 秒。初轮 Linux 52/53 和 Windows 28/29 均由新增错误码未加入校验表引起，失败记录保留。
- 复用 v2 夹具做真实 Windows→SSH→Ubuntu 历史恢复，A/B/C 三份 14 字节文件的内容与 SHA-256 全部一致。当前文件 SSH 下载的空、中文及多块文件一致，R1 SSH 上传和续传矩阵 12 组通过。所有联调使用专用根及临时端口。
- 已保留既有未提交工作，未提交或推送。原生保存框手动操作、原生下拉框专项、MSVC、硬件掉电与真实库升级未验证。详细文件、命令、日志和哈希见 [R2.3 交付](R2_3_HISTORY_RESTORE.md)。本轮停在 R2.3。

## R2.2 服务端历史版本元数据与分页查询（2026-11-04）

- 版本页可在本机提交记录与服务器历史之间切换。按逻辑路径查看序号、版本 ID、大小、SHA-256、提交时间及当前标记。异步查询支持更早页、取消及路径、端点切换。
- schema v3 在同一事务内建立并回填历史表，保留版本、块、当前指针和上传会话。旧时间为未知。首次数据库发布登记 UTC 秒及持久序号，复用版本 ID 保留原元数据。HELLO 新增 `version_history_v1`，帧 34/35，独占序号游标，默认 32、最多 64 条，控制帧 64 KiB。既有 `download_v1` 保留当前版本匹配语义。
- 保留旧服务端 `f00f601` 生成的 [v2 夹具](../tests/fixtures/README.md)。迁移验证覆盖回滚、提交前进程退出、重启、65 条分页、翻页中新增版本、旧客户端、旧库当前文件恢复及已确认块续传。实际查询的 EXPLAIN 命中路径/序号索引，未使用临时排序。
- Windows Qt 6.8.3 / MinGW 13.1 使用 `cmake -S . -B build-windows-gui`、`cmake --build build-windows-gui -j 2` 退出 0，`ctest --test-dir build-windows-gui -LE native-desktop --output-on-failure` **28/28**。Ubuntu 专用目录 `/tmp/chunkharbor-r22-src-WN6VOZ8F` 按既有依赖参数配置并构建，`ctest --test-dir build --output-on-failure` **52/52**。补充旧库恢复断言后迁移专项再次通过。首轮缺依赖参数及 48/52 失败记录保留。
- 真实 Windows→SSH→Ubuntu 历史查询核对旧库三个版本、当前哈希和未知时间，当前下载回归的空、中文、多块文件均匹配 SHA-256。使用专用根和临时端口，原工程与真实备份数据未被升级。
- 独立审查未发现遗留阻塞问题。旧库已在盘上改名但尚未完成数据库发布的升级边界未用该夹具专项验证。原生下拉框、MSVC、掉电和真实库升级未执行。精确命令、失败原因及文件列表见 [R2.2 交付](R2_2_VERSION_HISTORY.md)。未提交或推送。下一项最小任务为 R2.3 指定历史版本读取及恢复。

## 总览与顶部连接检测（2026-11-04）

- 总览使用四张等宽统计卡片和等高的最近备份、最近活动列表。统计仍取 WorkspaceStore 的真实本机记录和 TransferController 的任务状态。两个列表保留最近五条的范围，按现有顺序展示，时间经 Qt 解析和本地化，路径及活动文本可悬停查看。继续使用窗口现有传输栏。
- 顶部读取当前设置中的主机与端口，IPv6 显示带方括号的地址。地址可省略，完整值通过提示查看。检测入口由既有 HealthState 驱动，未检测、检测中、可达和失败四态各自对应按钮与颜色。总览和顶部从同一 Transport 读取状态。顶部和设置入口共用检测中防重入判断，修改端点后原探测与定时器按既有逻辑失效。
- 每次探测异步 TCP 建连并交换 HELLO，结束即关闭本次 socket。原有 5 秒超时及 15 秒复查沿用，失败提示来自实际 Transport 状态。QObject 所有权和线程归属沿用原窗口，改动范围是 Qt 展示与信号绑定。
- 本轮修改 MainWindow.cpp、DashboardPage.cpp、QtTcpTransport.cpp 的状态文案、Light.qss、gui_health_test.cpp、gui_pages_test.cpp 和本文件。保留已有未提交修改，暂存区未新增内容。
- Windows Qt 6.8.3 与 MinGW 13.1.0 使用 `cmake -S . -B build-windows-gui -DCMAKE_BUILD_TYPE=Debug -DCHUNKHARBOR_BUILD_SERVER=OFF -DCHUNKHARBOR_BUILD_CLI=OFF -DCHUNKHARBOR_BUILD_GUI=ON -DCHUNKHARBOR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=D:/QT/6.8.3/mingw_64` 配置退出 0。随后 `cmake --build build-windows-gui -j 2` 退出 0，日志为 `build-windows-gui/dashboard-build-final.log`。`ctest --test-dir build-windows-gui -LE native-desktop --output-on-failure` 27/27 通过，耗时 132.40 秒。日志为 `build-windows-gui/dashboard-ctest.log`。
- 首轮新增健康测试在旧状态文案上失败。首轮原生截图发现固定时间列宽受字体样式影响而截断，改为按渲染内容测量。
- 原生 Windows 125% 运行 gui_pages_test 通过。Qt 离屏 150% 运行同一测试通过。原生 150% 的完整页面测试先停在既有文件表格高度断言，随后用 `QT_QPA_PLATFORM=windows`、`QT_SCALE_FACTOR=1.2` 和 `chunkharbor-gui-pages-test.exe --dashboard-layout build-windows-gui/dashboard-overview-150.png` 单独验证总览，实际 DPR 1.5，退出 0。增加这个仅用于测试的选项后重新构建，默认 gui_pages_test 仍通过。
- GUI 回归覆盖真实 HELLO、检测按钮禁用、定时复查、超时与重试、端点失效、顶部和总览状态一致，以及空历史、最近记录、长路径和 IPv6 地址展示、窗口缩放、设置跳转。截图 `dashboard-overview-final.png` 和 `dashboard-overview-150.png` 来自隔离测试数据。真实 SSH、跨物理显示器拖动、Linux 构建及既有 native-desktop 下拉框专项本轮未执行。
- 下一项最小验收是在真实本机记录与实际 SSH 隧道环境中人工核对连接失败提示和跨屏缩放。

## UI-01 版本历史三栏与共享浏览样式（2026-11-04）

- **任务 / 审计**：用户明确授权版本历史 UI 重构及两页共用视觉规范；先核对根目录规则、指南、路线图、Git、README、进度、WorkspaceStore、BackupWorker、VersionsPage、FilesPage、MainWindow、QSS和测试。保留此前所有未提交的R2.1/文件页改动，不reset/clean/commit/push，不触碰真实备份。当前历史数据是本机成功提交记录，最多500条，按端点与逻辑路径分组，绝非服务器完整历史。
- **布局与共用规范**：版本页横向QSplitter分为约260px本机文件、弹性历史列表、约320px详情；最小窗口沿用1060×700，详情可手动收起。文件行42px，路径省略并以转义Tooltip查看完整值，禁水平滚动；历史三行展示本地时间、变化/大小、短ID；详情自然排列，保留版本/基线/内容SHA区分及实际变化块数与字节说明。移除底部详情和禁用恢复按钮，改轻量M6规划提示；不实现历史恢复。主窗口仅对版本页去掉外部QScrollArea，继续复用现有传输栏。
- **共用组件 / C++取舍**：新增header-only `widgets/BrowserUi.h` 统一列表、空状态、详情字段、完整文本省略/复制、splitter和页面间距；Light.qss以visualRole/role统一选中色、边框、圆角、复制按钮和字体，删除文件页专属重复规则。FilesPage复用并补同样的空列表/无匹配提示，原下载交互不变。QObject父子所有权和带上下文的连接自动管理UI生命周期；没有线程、socket、磁盘或控制器所有权变化。
- **绑定与时间**：QStandardItemModel保留原五列及完整FileRecord角色数据，详情只读当前选中行；无效选择/切文件/筛选无结果清除旧详情。时间解析ISODateWithMs后toLocalTime显示，原UTC/offset字符串不修改；按实际时刻稳定排序，修正原字符串排序对时区偏移和同秒毫秒的风险。缺失时区显示未知，不默认为UTC。基线absent仅显示首次提交提示且不可复制成ID。changedBlocks/missingBytes仍来自提交时上传计划，不是重新比较历史版本。
- **保护边界**：未改WorkspaceStore存储、备份流程、差异算法、版本/基线生成、协议、schema、上传/下载控制器及SHA校验；无需迁移。README同步说明本机三栏与恢复规划提示。完整11个文件清单及说明见 [UI_01_VERSIONS.md](UI_01_VERSIONS.md)。
- **实际配置 / 构建**：Windows Qt6.8.3、MinGW13.1.0，PATH前置Qt与MinGW bin；`cmake -S . -B build-windows-gui -DCMAKE_BUILD_TYPE=Debug -DCHUNKHARBOR_BUILD_SERVER=OFF -DCHUNKHARBOR_BUILD_CLI=OFF -DCHUNKHARBOR_BUILD_GUI=ON -DCHUNKHARBOR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=D:/QT/6.8.3/mingw_64`退出0；`cmake --build build-windows-gui -j 2`最终退出0，日志 `build-windows-gui/ui-01-build-delivery.log`；无新增C++编译警告，配置仍提示可选Vulkan头缺失，windeployqt仍提示dxcompiler/dxil缺失，未阻塞构建。
- **已执行专项**：`ctest --test-dir build-windows-gui -R '^(gui_pages_test|versions_page_test)$' --output-on-failure` 2/2通过。先在旧实现复现“缺少紧凑版本列表”失败，再实现。新增versions_page_test覆盖旧版/新版绑定、UTC与offset换算/真实排序、版本及基线复制、不同端点同路径分组、无效选择/无匹配清理、持久记录原值保留、长中文路径、宽窄/最大化、详情折叠和两页视觉role一致性。原生截图先发现delegate换行被连成一行及浅蓝选中背景上系统白字，分别修正明确分行绘制和使用文本palette；不改业务。
- **最终完整回归**：`ctest --test-dir build-windows-gui -LE native-desktop --output-on-failure` **27/27通过，146.93秒，退出0**；日志 `build-windows-gui/ui-01-ctest.log`。包含新增版本页、原文件页/查询、29个下载场景、输出安全/故障、生命周期、上传及健康回归。`git diff --check`退出0；暂存区仍为空。
- **原生 / 缩放验证**：以 `QT_QPA_PLATFORM=windows` 分别运行 `./build-windows-gui/chunkharbor-versions-page-test.exe` 与 `./build-windows-gui/chunkharbor-gui-pages-test.exe`，均退出0，并通过图像工具核对测试生成的 `ui-01-versions-final.png`、`ui-01-files-final.png`；使用长中文路径、完整32位版本ID和64位哈希的隔离夹具，无真实用户历史。以 `QT_QPA_PLATFORM=offscreen`、`QT_SCALE_FACTOR=1.5` 和 `2` 分别运行versions-page-test均退出0，正常/窄窗、折叠、最大化和绑定断言通过。原生与offscreen验证不同，未宣称多物理显示器实测。
- **未执行 / 风险**：原生保存框、不同物理显示器的DPI拖动、MSVC/Release、Linux编译/CTest、SSH真实端到端本轮未执行，理由是本轮UI范围且本地Qt回归覆盖业务接口。既有native-desktop下拉框专项此前失败且获用户人工确认，本轮未重跑/未禁用；不称全部原生专项全绿。高缩放的offscreen虚拟屏小于既有最小窗口不作为正常物理桌面验收；支持尺寸以下的屏幕需人工核对。
- **下一项最小任务**：用户在真实本机记录上人工核对两页、复制和高DPI显示；本轮不推进R2历史恢复协议或后端功能。

## 文件页面独立 UI 重构（2026-11-04）

- **范围 / 审计**：本轮由用户单独授权，只整理 Windows Qt 文件页。开始时 9 个已修改文件、11 个未跟踪文件、暂存区为空；包含尚未提交的 R2.1 下载实现和测试。全部保留，无 reset/clean/commit/push。核对了协议分层、路线图、README、进度、MainWindow、FilesPage、ServerFilesModel、FileListController、DownloadController、DownloadOutput 和现有测试；未重做里程碑。
- **布局**：保留导航与浅色 QSS，导航宽度 188px；文件页取消外层 QScrollArea，横向 QSplitter 将弹性列表与约 320px 详情并列。42px 行高，普通窗口可显示约 10 行，增加高度直接扩展列表。详情可拖动调宽或按钮收起；版本/哈希列仅隐藏，完整字段保留。页面内代理模型格式化大小，服务器原始整数未改；详情按宽度省略，PlainText 与转义 Tooltip 防止服务器字符串被解释为富文本，支持复制完整路径/版本/哈希。
- **状态 / 兼容**：搜索仍只筛选当前页；分页移至底部，不展示伪总数/总页数。FileListController 仅增加成功操作对应的展示页码，失败/取消不推进，端点改变重置；原游标、128 页返回边界与查询协议不变。过滤/页面重置清理无效详情。传输栏从详情移到主窗口内容底部，页面切换后仍显示；直接绑定原 DownloadController 的独立任务元数据与 changed 信号，取消仍调用原 cancel。未改下载状态机、哈希、Win32 安全写入/发布、上传、服务端、schema 及关闭语义，无数据迁移。
- **线程 / 所有权**：GUI 线程只渲染与交互；QObject 父级负责 Widget 生命周期，MainWindow 重新挂接传输栏，原窗口所有的 DownloadController 保持存活。既有后台 socket/哈希/文件操作与 queued 状态机制不变。未操作真实备份目录。
- **实际 Windows 配置**：`cmake -S . -B build-windows-gui -DCMAKE_BUILD_TYPE=Debug -DCHUNKHARBOR_BUILD_SERVER=OFF -DCHUNKHARBOR_BUILD_CLI=OFF -DCHUNKHARBOR_BUILD_GUI=ON -DCHUNKHARBOR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=D:/QT/6.8.3/mingw_64` 退出0。PATH 前置 `D:/QT/Tools/mingw1310_64/bin`、`D:/QT/6.8.3/mingw_64/bin`；复用 Qt6.8.3 / MinGW13.1.0 / Ninja 既有构建目录。`cmake --build build-windows-gui -j 2` 退出0，无新增 C++ 编译警告；Qt 配置仍提示可选 Vulkan 头缺失，windeployqt 提示 dxcompiler/dxil 缺失，未影响构建。
- **实际回归**：`ctest --test-dir build-windows-gui -LE native-desktop --output-on-failure` **26/26 通过，139.00秒，退出0**，日志 `build-windows-gui/files-ui-ctest-final.log`；包含 29 个下载 TCP 场景（新增更换选中项并切换页面仍下载原任务）、21 个文件查询场景、SHA/输出故障/取消/在途销毁及上传回归。没有删减原异常断言。
- **测试迭代记录**：先在旧布局复现列未隐藏失败；新长路径夹具未排序被原协议验证拒绝，修正夹具。首轮非原生 CTest 25/26：no_capability 模拟端过快返回，10ms 心跳未触发；仅为该模拟响应加40ms延迟，保留心跳断言，重跑通过。原生最大化测试增加100ms事件循环等待窗口管理器完成几何调整。一次补编与运行中的测试撞到 exe 占用，退出后重新构建；不终止用户 GUI 进程。失败日志保留 `files-ui-ctest.log`。
- **最终补验**：最新源码 `cmake --build build-windows-gui -j 2` 退出0（`files-ui-build-delivery.log`）；`ctest --test-dir build-windows-gui -R '^gui_pages_test$' --output-on-failure` 1/1通过，退出0。设置 `QT_QPA_PLATFORM=windows` 执行 `./build-windows-gui/chunkharbor-gui-pages-test.exe` 退出0，最大化窗口2048×1081逻辑像素、表格viewport高678。设置 `QT_QPA_PLATFORM=offscreen` 分别以 `QT_SCALE_FACTOR=1.5`、`2` 执行同一测试，均退出0；正常窗口与窄窗口完整断言通过，虚拟屏最大化低于支持最小尺寸时仅验证内容可见，不当作物理高DPI验收。`git diff --check` 退出0。
- **视觉 / 限制**：使用 windows 平台检查中文、长路径、省略、选中色、复制、窄窗/普通/大窗口/最大化和详情收起；截图 `build-windows-gui/files-ui-windows-final.png` 为32条长路径测试夹具，非真实服务器备份。offscreen 字体不能代表 Windows，截图采用原生 windows 平台。高缩放下 offscreen 虚拟屏小于既有 1060×700 最小窗口，不据此宣称小屏幕最大化体验已验收。
- **未执行**：既有失败且此前获用户人工确认的 native-desktop 下拉框专项未重跑，未禁用；不称全量27个测试全绿。Windows 原生保存框人工选择、不同物理显示器/DPI 的人工拖动、MSVC/Release、Linux 构建/CTest及本轮SSH端到端未执行；本轮未改相关后端。保留64MiB、单任务、无下载续传和不覆盖限制。建议下一轮最小任务是用户人工核对真实数据的文件页与原生保存框；R2.2需另行授权，不自动继续。
- **文件与设计**：完整本轮14个文件清单、Qt布局和控制器关系见 [FILES_PAGE_UI.md](FILES_PAGE_UI.md)。

## R2.1 Windows Qt GUI 当前单文件下载（2026-11-04）

- **范围与基准**：从Windows/Ubuntu现有 `f00f601` 继续；保留本文件本轮之前未提交的清理/恢复记录。仅实现R2.1，不新增服务端协议、SQLite schema、历史/目录下载或GB级能力，不改健康检查和auto备份策略，不提交/推送/跨机同步源码，不操作真实备份。
- **交付**：文件页选中条目→下载→系统保存框→后台逐块读取与校验→安全发布；显示文件名、字节、百分比、状态及取消。保存框前固定端点和列表的路径/版本/大小/摘要，VERSION_CONFLICT提示刷新，不静默选择新版本。空文件也校验SHA-256；失败/取消不发布半文件，既有目标拒绝覆盖，重复启动拒绝。
- **线程/资源**：新增DownloadController、DownloadOutput；worker独占QTcpSocket、哈希和Win32输出，GUI只处理queued状态；每次至多256KiB、一个在途请求和一个待处理进度事件。5秒连接、10秒请求期限，每50ms检查取消。原子CAS定义取消与发布的提交点；越过提交点后以发布结果为准。正常关窗异步取消，直接析构先停止既有列表/健康连接、销毁页面，再取消/join任务，避免拆卸回调访问已销毁成员。
- **Windows发布**：本地绝对路径与服务器逻辑路径分离；逐祖先拒绝reparse并保留不共享DELETE的目录句柄；同目录随机临时文件CREATE_NEW、独占句柄，块/整文件SHA-256一致后FlushFileBuffers和FileRenameInfo（ReplaceIfExists=FALSE）发布。已有目标检查只是提前报错，真正防竞争由系统调用保证；RAII通过文件句柄删除未发布临时对象。仅本地盘路径且长度<240字符，UNC/设备/ADS/保留名等明确拒绝，无覆盖回退。
- **测试先行及修复记录**：gui_pages_test先复现“选择有效文件仍不能下载”。初轮构建误并发导致静态库文件锁冲突，已改串行构建；探针中文argv误作UTF-8导致路径打开失败，改用Qt Unicode arguments。首轮完整CTest实际24/26：combo_popup_test历史原生输入失败、gui_files_test关窗堆损坏；修正页面/控制器销毁顺序后列表通过。随后回归24/25中gui_health_test析构崩溃；GDB定位socket析构→disconnected→fail→已销毁QTimer::start，新短生命周期测试先复现SegFault，再在MainWindow销毁前调用既有停止接口修复。原始失败日志全部保留，不宣称首轮全绿。
- **构建/最终自动验证**：Windows Qt6.8.3 / MinGW13.1.0，使用 `D:/CMake/bin/cmake` 与原Ninja缓存；配置和构建命令见 [R2.1专项](R2_1_GUI_DOWNLOAD.md)。`cmake --build build-windows-gui -j 2` 退出0。最终 `ctest --test-dir build-windows-gui -LE native-desktop --output-on-failure` **26/26通过**，136.89秒、退出0；包含10轮健康/列表在途销毁测试。日志为 `build-windows-gui/r2-1-{build,ctest}-delivery.log`。`git diff --check` 退出0。
- **新增自动覆盖**：gui_download_test含28个TCP/GUI场景（含空/普通/多块/中文、两级SHA错误、版本冲突、断连/超时、部分下载取消/断连、重复启动、关窗/销毁、已有/竞争/硬链接目标、junction父目录、目录替换及畸形响应）；download_output_test在测试编译单元注入真实Win32写入/flush失败和短写，验证不发布及清理；gui_download_lifetime_test覆盖健康/列表等待HELLO时的销毁各5轮。生产代码无测试故障开关。
- **真实SSH下载**：`python tests/integration/windows_gui_download_ssh_test.py build-windows-gui/chunkharbor-gui-download-probe.exe miniob-ubuntu /home/ning/ChunkHarbor/build/chunkharbor-server` 退出0。通过真实文件列表选择、保存框、下载；0字节空文件、19字节中文文件和793856字节多块文件，服务端files副本与Windows结果大小/SHA-256逐项一致。最终专用根 `/tmp/chunkharbor-r2-1-download-5taCk4mr`，日志 `build-windows-gui/r2-1-ssh-download-delivery.log`；临时测试服务/隧道已停止，本地结果随隔离临时目录清理。
- **R1上传回归**：`python tests/integration/windows_mode_ssh_test.py build-windows-gui/chunkharbor-gui-transfer-probe.exe miniob-ubuntu /home/ning/ChunkHarbor/build/chunkharbor-server` 退出0，12组真实SSH矩阵通过，涵盖full目标保护、incremental/无变化、显式resume、混合auto、检查点损坏/过期、源/远端基线变化、部分成功后重扫确认及活跃目标保护。专用根 `/tmp/chunkharbor-mode-ZsXHw2YB`，日志 `build-windows-gui/r2-1-ssh-upload.log`；不修改或重启用户服务。
- **审查与未验证**：独立只读审查已复核Win32发布、CAS取消、queued回调和最终shutdown顺序，未报告遗留阻塞问题；不以静态审查冒充运行。自动化保存框强制Qt非原生，正式程序默认Windows原生保存框的人工交互未验证；MSVC/Release、硬件掉电、强杀恢复、真实磁盘耗尽未执行（写入/flush失败为注入测试）。未重跑Linux完整构建/CTest，服务端未改且复用既有二进制完成SSH验收。R1原生combo_popup_test失败仍保留，未修复/删除/禁用，不称全部CTest全绿。
- **兼容与限制**：原download_v1版本前置条件、上传/持久会话及schema v2不变，无迁移；单文件64MiB、单任务，无下载续传，越过发布提交点的取消可能报告成功；Windows数据flush不等于已验证掉电下目录元数据持久性。Linux Qt GUI安全输出后端本轮不提供，明确提示用Linux CLI。专用测试根保留用于审计，无历史GC或真实数据删除。
- **下一项唯一最小任务**：R2.2服务端历史元数据/时间语义、旧库兼容迁移与有界查询设计；本轮停止，不自动开始实现。详情见 [R2.1设计与验证](R2_1_GUI_DOWNLOAD.md)。

## 用户授权清理可再生成的测试产物（2026-11-03）

- 用户指定删除 `/tmp/chunkharbor-startup-audit-4pkwba53` 隔离副本，并选择仅清理可重新生成的测试产物和临时文件。已检查临时路径、非符号链接及所有者后删除该副本；不删除测试源码、真实备份或备份数据库。
- Windows `build-windows-gui` 删除24个测试可执行程序/探针，共340,872,359字节；Ubuntu原工程 `build` 删除34个测试可执行程序/故障注入库，共86,054,176字节。跳过运行中文件检查无命中；清理前后GUI、server、CLI及各自CMakeCache的SHA-256保持一致。
- 保留原始测试日志、截图和此前用户数据恢复/删除审计备份，这些是历史证据或数据备份。CTest测试程序现需先执行对应平台 `cmake --build` 重建；本轮没有重跑测试，以免立即重新生成刚清理的产物。未提交或推送。

## 用户授权删除 photos 当前备份（2026-11-03）

- 用户明确要求删除Linux备份根 `files/photos` 以释放空间。只读审计发现47条当前路径、47个版本，文件目录约323MiB；无该路径在途上传、无legacy迁移记录，其他当前路径未引用这些版本。
- 先在隔离副本删除47条current_files记录和对应目录，SQLite完整性/外键检查通过，服务端随机端口启动并完成HELLO；50条其他当前记录及101个版本保留。
- 真实操作获取存储根flock，核验47个目标文件及SHA-256、精确目录内容、所有权和无符号链接；先备份SQLite并保存清单到 `/tmp/chunkharbor-delete-photos-audit-zkww3ku4`，事务移除47条当前路径，再删除用户明确指定的 `files/photos` 目录。剩余50个当前文件逐一核验SHA-256、SQLite完整性和外键均通过；101个历史版本保留，未修改其他路径。
- 删除的是当前文件目录（约323MiB逻辑大小），`.versions` 历史副本仍占空间；本轮未自动清理历史版本。真实服务端未启动验证，以免启动恢复流程影响其他会话；隔离副本已验证启动。未修改业务代码，未commit/push。

## 用户授权恢复缺失备份文件（2026-11-03）

- 用户明确授权恢复真实备份根 `/home/ning/exchangeFiles/chunkharbor-backup` 中缺失的旧路径文件。恢复前核验无运行服务、SQLite quick_check、97条当前记录、路径/权限及版本副本SHA-256；缺失数量为47。
- 仅补齐47个缺失文件，临时文件校验、fsync后以不覆盖已有目标的方式发布；目录0700、文件0600，逐个检查大小和SHA-256。恢复后97个当前文件校验一致，恢复操作期间数据库哈希未变；审计清单位于Linux临时目录 `/tmp/chunkharbor-real-recovery-audit-llsdd1tw/manifest.json`。
- 原备份根上临时启动服务端随机端口36737成功，CLI HELLO退出0；测试服务已停止，用户可自行在8787启动正式服务。首次脚本在任何写入前因pgrep无进程返回1而退出，修正检查后成功。未修改项目业务代码，未提交或推送。

## 发布后现场排查：存储初始化失败与SSH转发拒绝（2026-11-03）

- 用户报告服务端`storage operation failed`及SSH本地转发反复`connect failed: Connection refused`。现场检查无ChunkHarbor服务进程、Ubuntu8787无监听；源码先构造UploadStorage/DeltaStorage，之后才创建TcpServer，故初始化失败会使SSH转发的目标连接被拒绝。
- 只读核对原备份：SQLite quick_check正常，空间/inode充足、根目录权限正常；97个当前文件中47个在files下缺失，其他当前文件摘要核验通过；47个对应不可变版本副本均存在且大小/SHA-256匹配。尚不能判断缺失文件被删除/移动的时间或责任来源。
- 独立副本`/tmp/chunkharbor-startup-audit-4pkwba53/storage`复现：strace确认读取首个缺失当前文件返回ENOENT，进程退出2。只在副本恢复47个文件后，初稿恢复文件/新目录默认权限过宽，被安全校验拒绝；gdb定位directory权限检查，修正副本目录0700/文件0600后成功LISTENING随机端口36251，CLI HELLO退出0。测试服务已停止，副本保留用于核对。
- 原备份未恢复、未删除、未重置数据库；只读检查与副本验证前后原数据库SHA-256一致。未修改业务代码、Git提交/推送或真实服务状态。实际数据恢复需用户另行明确授权，不能把副本启动成功写成真实服务已经恢复。

## R1 发布与 Linux 原工程同步（2026-11-03）

- 用户明确授权提交、推送GitHub并同步Linux虚拟机。R1功能提交为 `83453c4ba134719af4f7ded3e875c986fd8d3488`；发布文档随后作为独立提交，最终版本以Git HEAD核对为准。
- Windows Qt构建成功；`ctest --test-dir build-windows-gui -LE native-desktop --output-on-failure` **23/23通过**。原生鼠标测试本轮未运行；此前完整CTest为23/24，用户已人工确认下拉框，失败记录仍保留。
- Ubuntu `/home/ning/ChunkHarbor` 使用Git bundle将原工程从 `31e5d3a` 快进到R1功能提交，复用原build，GUI=OFF。配置和构建完成，`ctest --test-dir build --output-on-failure` 日志显示 **51/51通过**（38.82秒）。远程包装命令在测试结束后因shell退出码转义错误返回非零；独立检查构建与完整CTest日志确认上述结果。
- 同步前Ubuntu七项未提交改动已归档于 `stash@{0}`，对象 `612176dd803aa076c0ca688ead0d4e7d88f2f14a`；逐文件比较规范化SHA-256，审计保存在原仓库 `.git/codex-pre-r1-publish-audit.json`，此前stash保留。构建目录及真实备份数据不进入提交；未重启用户服务。
- GitHub main 和两端最终提交需发布后核验；本轮没有启动R2。具体阶段验收例外和历史测试详见下方R1记录。

## R1 阶段完成并关闭（2026-11-03，用户确认验收）

- **完成状态**：用户已自行验证下拉框，确认未发现实际交互故障，并明确要求结束R1剩余事项。结合前轮源码、构建、自动化与真实SSH证据，R1按当前约定范围完成；下方各轮“未验收/剩余”描述保留为当时历史，以本条最新状态为准。
- **已交付**：独立GUI健康检查与可达性误判修复；磁盘工作期间误判空闲的上传断连修复；ComboBox圆角底框修复；严格手动模式与检查点保护；auto逐文件Full/Incremental/Resume/Skip/Error计划、分类统计和安全部分执行；配置/源/远端基线绑定、失效预览及重试重新确认。保持协议、历史、会话格式和串行架构。
- **验收证据及例外**：前轮Windows构建成功，最新完整CTest实际为23/24，ComboBox原生自动输入专项仍失败；此记录不改为24/24。用户人工自测作为下拉框功能验收补充，按用户明确决定不再将该自动化输入问题作为R1阻塞项，不删除/禁用测试，也不认定失败必然由用户鼠标操作引起。业务模式、上传/崩溃恢复、A/B重扫三项真实SSH回归已有通过证据；服务端修复轮Linux51/51属于该轮证据，不冒充本轮新测。
- **旧截图关闭**：按用户要求停止追查约10.9MiB照片的原始断连，不再作为R1待办；瞬时网络波动仅为可能解释，未将其写成已确认根因。已确认的空闲回收缺陷修复、隔离故障测试与安全恢复证据保留。
- **本轮修改/验证**：仅整理PROGRESS、ROADMAP_V3、README及R1专项入口，不改业务/测试代码或用户数据。实际检查Git状态、既有测试记录、Markdown链接、文档差异及历史正文完整性；本轮不重新构建、不重跑CTest、不启动原生鼠标测试，沿用上述已标明轮次的证据。
- **保留边界**：64MiB文件限制、可信loopback/SSH、无GUI恢复/鉴权等仍按后续阶段规划；Linux Qt/MSVC/Release/Sanitizer、硬件故障、长期压力及未提供的人工输入/DPI组合不补写为通过。跨机器同步、提交与发布不等于阶段功能验收，本轮未执行。
- **停止点**：R1无继续开发的必做子任务。R2仍为规划中，待用户明确启动；不自动继续，不commit/push，保留所有既有未提交改动与M0–M7历史。

## R1 自动备份与文件级策略计划实现完成（2026-11-03；GUI原生输入回归仍有失败）

- **范围/流程**：本轮按用户新授权新增auto默认策略，不再要求混合目录统一手动模式。目录配置与进度由既有BackupConfig/TransferController管理，FileRecord新增内存BackupStrategy，逐文件Full/Incremental/Resume/Skip/Error，保持原串行worker及独立Peer。变更表新增执行策略列，原说明区显示准确分类、错误及预计上传量；没有并行/FastCDC/下载/认证/Redis/服务端重构。
- **安全规则**：优先恢复本机身份匹配且QUERY确认的未完成会话；Absent→Full、已存在且变化→Incremental、Unchanged且缺失0→Skip。auto不受仅用于手动full/incremental的resume布尔开关关闭影响；显式resume仍必须有效会话。无效/损坏/源不匹配状态保留并标Error，本轮保守拒绝，不自动清空后重传。已提交回执丢失时核验真实COMMITTED结果恢复历史，再规划Skip，不误当未完成会话。
- **预览权限/隔离**：新增RunResult::planComplete，仅完整扫描才可在auto下确认安全子集；Error pin执行时固定不上传，最终明确部分失败。全局预检错误、取消、目录集合变化不授权；补红测修正“已有排除行时全局失败仍展示可确认计划”的提示，按钮本来就禁用。模式/目录/前缀/设置变化仍清空预览，任务结束清空pins，重试先重扫再确认。执行复核源SHA/大小/路径、固定Absent/Exact及会话ID/状态；Resume额外进行固定基线Preview，错误时发送0数据块。不改变ACK/COMMIT持久性语义，Skip不创建重复版本。
- **兼容/文件**：新用户默认auto，保存的full/resume/incremental不改写；auto新上传仍写旧full/incremental检查点语义，恢复旧会话保留其原模式。新增字段不进历史/检查点序列化，无协议/schema迁移。生产仅TransferTypes、BackupWorker、TransferController.cpp、TaskModel、BackupPage、SettingsPage；扩展worker/state/pages/layout单测、gui_transfer_probe及原windows_mode_ssh矩阵，未改本轮CMake、健康检查、ComboBox/QSS、共享库或服务端。新增[R1_AUTO_BACKUP](R1_AUTO_BACKUP.md)，更新BUILD_WINDOWS、GUI_DESIGN及前轮模式文档入口，完整旧轮记录保留。
- **Windows构建/CTest实际结果**：Qt6.8.3/MinGW13.1/Ninja/Debug；`cmake -S . -B build-windows-gui`、`cmake --build build-windows-gui -j 2`成功。最终业务/提示代码的完整`ctest --test-dir build-windows-gui --output-on-failure` **23/24通过，110.63秒**（非性能指标）；仅`combo_popup_test`在`mouse opens actual popup`失败13.07秒，随后独立`-R '^combo_popup_test$'`仍0/1失败2.39秒。该测试同轮另一次跑到全部通过28.90秒，但不能据此宣称最终全量通过。其余模式、worker、健康、列表、布局和协议均通过，新增全局错误红测修复后gui_pages通过。既有Vulkan头、dxcompiler/dxil部署告警非阻断。
- **真实SSH业务矩阵通过**：最终探针运行`windows_mode_ssh_test.py`（原严格手动矩阵+新增auto）退出0，根`/tmp/chunkharbor-mode-Rfj1b0j7`。覆盖Full/Incremental/Skip不增版本、旧full会话被auto恢复、关闭手动开关仍自动恢复、auto检查点被手动resume读取、丢失提交回执恢复Skip、混合四策略、损坏JSON/失效会话保留原字节、仅错误不授权/安全行部分成功、源变化、配置三类变更、部分成功重扫/重试、扫描后会话失效在DATA前拒绝、无在途任务时远端新版本使旧Skip失效；只读SQLite版本/指针/accepted数量及服务端SHA-256断言通过。已查看最终真实MainWindow探针截图`build-windows-gui/auto-advice.png`，当前DPR1.25下四策略文字及分类预计量可见。
- **原有真实SSH回归**：`windows_gui_ssh_test.py`本轮worker业务实现后通过，根`/tmp/chunkharbor-m5-data-L804Bpbt`，含GUI kill+server SIGKILL后恢复、中文/空文件/排除、单块变化、暂停取消和关窗；此后仅策略列显示及全局失败文案修正。最终探针`windows_gui_rescan_test.py`退出0，根`/home/ning/ChunkHarbor/build/rescan-data-722wYAdS`，覆盖A/B切换、重复扫描、锁定/缺失源、修改源保留状态及另目标上传；其夹具由原脚本带路径检查清理。精确命令见专项文档，测试服务/隧道已结束，未接触真实备份。
- **失败与审查**：初始auto/策略列2个真实红测失败；测试初稿变量作用域错误先修夹具才取得行为失败。竞态夹具想让另一任务提交在途目标，实际被服务端target-busy拒绝，调整为断言该保护并分开验证真正远端更新；续传基线错误用本地可控TCP验证0DATA，不冒充真实server并发复现。中途全量22/24，另有既有文件列表cancel夹具unexpected client disconnect；随后最终该项通过。一次重扫与链接探针重叠报Windows文件占用，纠正顺序重跑通过。只读独立审查按本轮前快照核对，未发现确定新安全问题；审查者未自行测试。`git diff --check`及原进度正文比对通过。
- **未验证/风险/停止点**：未同步/重编译Ubuntu、未跑Linux全量、Linux Qt/MSVC/Release/Sanitizer、旧GUI二进制、硬件故障/长期压测、物理人工输入或本轮多档DPI。原生ComboBox自动输入失败仍未定位，不能称全部GUI回归通过；本轮不越界修复。64MiB/原超时/串行模型保留，额外续传Preview有哈希/清单成本，不宣称性能提升。无法关联的损坏状态仍是全局错误；无本机检查点/端点迁移不自动接管会话。R1全阶段未验收。未commit/push，保留前轮未提交改动，本轮停止，不自动推进R2。

## R1 备份模式校验与扫描建议完成（2026-11-03）

- **原行为/根因**：full保护Absent目标，incremental用固定块增量，resume复用持久会话；但原`BackupConfig::resume`自动开关同时控制显式resume，无检查点/关闭开关会静默转为新上传。full预览的Absent限制不能提供已有版本的差异建议；无变化仍可能提交同内容版本。页面配置变更仅禁按钮，控制器预览资格及retry存在绕过/旧基线风险。
- **最小实现**：扫描用真实Preview/Auto和QUERY_DELTA，核验检查点源身份、大小、SHA-256、端点、路径、块大小、会话类型/阶段/基线；剩余量取服务端位图。新增内存BackupAdvice/modeAllowed/resumeSession，预览区显示首次/增量/续传/无需上传/待核验分类及不适配原因，不静默改模式。显式resume必须有效会话，自动开关仅控制full/incremental；full已有目标拒绝（本任务完成记录匹配且当前内容一致可安全跳过）。无变化执行固定版本复核，不发DATA_CHUNK/COMMIT、不伪造versionId；损坏/失效/不匹配状态保留并拒绝。
- **预览/重试**：模式或配置改变清空控制器预览；执行前复核源与Absent/Exact基线。正式执行任何结果都使旧pins失效；retry按原配置重扫，用户确认后再开始。真实红测确认旧retry在A成功/B取消后使A冲突而B继续提交；修复后只生成A无需上传/B续传预览，B未确认不发布，再执行A版本不变、B哈希一致。独立静态审查确认该P2已解决，未发现其他确定的模式安全问题；审查者未自行运行测试。
- **修改文件**：生产BackupWorker、TransferTypes、TaskModel、TransferController、BackupPage、SettingsPage；worker单测、GUI探针及其CMake样式资源，新增windows_mode_ssh_test并更新windows_gui_ssh_test旧混合目录预期。新增[R1_BACKUP_MODES](R1_BACKUP_MODES.md)，更新BUILD_WINDOWS/GUI_DESIGN。未修改本轮健康检查、ComboBox、服务端、协议、schema/client_core；保留前轮未提交内容和下方M历史。
- **Windows实际构建/回归**：Qt6.8.3/MinGW13.1/Ninja/Debug，Kit在`D:/QT`；`cmake -S . -B build-windows-gui`、`cmake --build build-windows-gui -j 2`退出0。最终隔离串行`ctest --test-dir build-windows-gui --output-on-failure` **24/24通过，127.99秒**（非性能指标），含worker、健康、协议、列表、布局和原生ComboBox。之前全量曾23/24（ComboBox原生自动输入`mouse opens actual popup`失败）；不改组件单项复跑1/1通过28.94秒，最终全量也通过，但间歇性桌面输入原因未定位，不抹去失败。既有Vulkan头和dxcompiler/dxil部署告警非阻断。
- **真实Windows→SSH→Ubuntu三项通过**：`windows_mode_ssh_test.py`模式矩阵全部退出0，专用根`/tmp/chunkharbor-mode-p540xG4b`；覆盖全新/增量/无变化、错误full/resume、关闭自动开关的显式续传执行、源大小变化/同大小内容变化、损坏JSON/失效会话且记录保留、模式切换、部分成功重试、混合目录和执行前远端更新。只读SQLite验证版本数/指针及未确认不发布，逐文件SHA-256一致。既有`windows_gui_ssh_test.py`重跑通过（根`/tmp/chunkharbor-m5-data-JnIixQ6z`），含GUI kill+server SIGKILL后恢复、中文/空文件/排除、单块变化、暂停取消/关窗恢复；混合目录严格resume先拒绝无会话新文件，再由测试明确选incremental重扫确认。`windows_gui_rescan_test.py`通过，含A/B切换、反复重扫、锁定/缺失源和旧会话保护；其专用夹具由脚本清理。精确参数见专项文档。
- **失败及证据**：最初新增无会话resume单测在旧代码失败，修复后通过；QUERY Peer作用域初稿使单连接假服务端超时，缩短RAII作用域后通过。新SSH夹具初稿将状态目录与外部源重名，改独立states/后重测。旧端到端“混合目录resume静默上传新文件”的断言按新严格规则更新，而非放松生产保护。已查看`build-windows-gui/mode-advice.png`真实MainWindow探针截图，分类建议可见；测试同时断言可见文案及未自动改模式。`git diff --check`通过，既有PROGRESS从HEAD的M6发布记录起全文经换行规范化比对一致。
- **未验证/风险**：未同步/重编译Ubuntu、未重跑Linux全量（本轮无后端修改），未测Linux Qt/MSVC/Release/Sanitizer、硬件故障或长期负载；未做本轮人工操作、多档DPI。QUERY_DELTA不列出完整源身份，必须同时有本机检查点；丢失检查点不自动接管远端会话，转发端点变化不自动迁移。full拒绝已有目标而不新增覆盖对话框；批任务仍非跨文件原子。64MiB限制及原超时保留，无需上传仍会进行小型Preview协商。R1全阶段尚未宣称验收。
- **停止点**：本项完成；测试服务/隧道已结束，不触碰真实备份，未commit/push。下一轮由用户选择R1余项，当前可人工复核实际备份目录的分类建议与严格模式提示，不自动推进R2或其他功能。

## R1 ComboBox弹层圆角修复完成（2026-11-03）

- **根因**：原生Windows运行确认popup顶层透明/无边框/NoFrame展开后仍有效，但Qt私有容器额外绘制PE_PanelMenu，外角alpha=255，内层QSS圆角外出现矩形底框和平台阴影。新增真实MainWindow弹层测试先观察到像素断言失败，再改实现；不是只改QSS猜测根因。
- **最小修改**：仅生产ComboBox.h和Light.qss。保留QComboBox容器及输入机制，局部过滤popup Paint，透明清底后绘制统一圆角/边框/轻阴影；禁用原生附加阴影，列表/viewport不重复铺底。透明样式仅限定comboPopupList，不改通用QComboBox；给阴影留逻辑像素边距，面板与控件对齐、宽度/屏幕水平边界受控。无业务、网络/健康检查、协议、SQLite或其他页面布局修改。
- **测试与截图**：新增combo_popup_test.cpp及CMake目标（Qt6::Test仅测试链接，标记native-desktop串行）。备份/日志/设置5个下拉框，在普通/最大化、实际DPR1/1.25/1.5均验证四角透明、宽度/位置、鼠标、上下键、Enter、Escape、外部点击关闭和焦点返回；原生屏幕、整窗及独立alpha截图已查看，矩形底框消失、浅蓝高亮保持。产物位于忽略目录build-windows-gui/combo-artifacts。
- **实际命令/结果**：Windows Qt6.8.3/MinGW13.1/Ninja/Debug，`cmake -S . -B build-windows-gui`及`cmake --build build-windows-gui -j 2`成功；Windows QPA下QT_SCALE_FACTOR=0.8/1/1.2对应实际100/125/150，专项均通过，最终100/150专项分别28.75/28.76秒；取消比例覆盖后`ctest --test-dir build-windows-gui --output-on-failure`完整 **24/24通过**（125.77秒，非性能指标），含最终125%弹层和原健康/上传worker/列表等回归。完整设计、命令、失败与截图映射见 [R1_COMBO_POPUP](R1_COMBO_POPUP.md)。
- **失败/边界**：测试初稿findChild自定义无Q_OBJECT类编译失败，修夹具后才取得真实红测；直接QWidget合成点击被弹层防误触逻辑忽略，最终QTest QWindow输入路径通过。SetCursorPos返回false，未变更系统输入权限：已做真实Windows显示和自动事件验证，未做人工物理鼠标或全局OS缩放切换/混合DPI跨屏验收。Linux GUI/MSVC/Release/Sanitizer/其他Qt风格与版本未测；既有Vulkan与dxcompiler/dxil告警非阻断。独立静态审查无生产阻断问题，建议明确原生桌面测试要求，已标记。
- **范围/下一步**：保留前轮所有未提交改动及M历史；docs/PROGRESS已更新，未提交/push，不同步Ubuntu、不触碰真实备份。本轮仅此UI项，完成后停止；下一轮仍由用户选择R1可复现最小问题，不自动推进。

## R1 上传断连补充核验：约10.9MiB与默认30秒（2026-11-03）

- 用户提供中文路径JPEG约10.9MiB、其他文件继续、无暂停直接证据；仍无完整日志/等待时长。该大小未触发64MiB限制，不能将旧截图直接归因于大文件/暂停，也不能将此前缩短idle的复现自动等同原故障。
- **仅测试/记录变更**：新增windows_upload_disconnect_matrix.py；测试upload_delay_hook新增有界延迟/跳过块参数，默认仍2秒/0块。没有改生产代码、客户端/服务端30秒期限或任何保护。用11,429,478字节合成文件，前8块真实ACK后在第9块注入，私有根/随机端口/独占PID，不读取原照片。
- **实际默认配置5场景通过**：normal成功；28秒慢同步成功（整批36.58秒，marker和耗时断言确认）；32秒同步触发client原文server response timed out（整批33.52秒），后续文件成功；终止自建SSH隧道而server存活，以及SIGKILL测试server而隧道保留，均可产生server disconnected且后续文件连接也失败。脚本不传idle-timeout-ms，沿用server默认30秒。
- **恢复证据**：超时和隧道断开场景服务端保留9块，GUI真实查询后只补340块；server被杀后启动恢复保留8块，只补341块。checkpoint失败时completed=false、无成功version；恢复后的两个文件逐项SHA-256一致。未收到第9块ACK不能断言该块未落盘，必须查询持久会话。
- **关联构建/测试**：Ubuntu原工程`cmake --build build --target chunkharbor-upload-delay-hook -j 2`成功；`ctest --test-dir build -R 'application_pause_test|tcp_connection_test|upload_idle_integration|delta_fault_integration|resume_integration' --output-on-failure` **5/5通过**。Windows沿用原GUI二进制运行五模式真实SSH矩阵均退出0；精确命令/专用根/审查补强见[R1_UPLOAD_DISCONNECT](R1_UPLOAD_DISCONNECT.md)。本轮未重跑Linux51项或Windows23项全量，不列为本轮新证据。
- **结论/剩余**：30秒是单次请求等待，非整个文件上传时长。单文件捕获异常并继续下一Peer符合当前设计；持续SSH/server失效时后续也失败。原截图后续成功不太符合持续性整条隧道中断，但瞬时网络/SSH channel/单TCP关闭及默认阈值竞态仍未排除。缺同一连接的客户端请求ID/类型/耗时/socket错误、服务端关闭原因/配置/退出记录、SSH时间线和实际二进制信息，不能将server disconnected文本当作根因。
- 未做物理网络黑洞/丢包、真实磁盘长尾/硬件故障、原照片/原负载复现；不新增自动重连或延长期限。测试服务和隧道已结束、根保留，未触碰真实备份，未提交/push。本轮停止，仅在真实再次复现时继续补上传诊断，不推进其他功能。

## R1 第二项完成：磁盘工作期间被误判空闲导致上传断开（2026-11-03）

- **确认根因**：UploadHandler接受请求后pauseReading并投递有界DiskExecutor，但TcpServer定时器原只看lastActivity。排队/校验/同步超过idle阈值会在结果返回前关上传连接，BackupWorker独立Peer抛server disconnected。健康检查QtTcpTransport为另一个对象，本轮未改其代码。每文件独立Peer及try/catch使失败文件记错并保留会话，后续文件能在新连接继续；工作池仍堵时也可能连续失败。
- **复现证据**：新增仅测试LD_PRELOAD，首次.delta文件fdatasync前延迟2秒。旧Linux server idle500ms在持久确认前返回EOF，专项红测失败；真实Windows GUI→SSH→Ubuntu idle1500ms复现首文件server disconnected、下一文件成功，再Resume后两文件哈希一致。未取得用户原始日志/时长，不能断言历史所有断开都同源；生产默认30秒阈值未改。
- **最小修改**：仅两个生产文件TcpConnection.h、TcpServer.cpp；新增idleTimeoutSuspended=applicationPaused且非readEof，回收暂跳过服务端业务等待。普通空闲、输出高水位背压、shutdownAfterWrite错误排空仍可回收。完成后原send更新活动时间、queueInLoop恢复读取；ACK/SQLite/协议、线程归属、队列上限和客户端超时不变，没有吞异常/伪成功/添加自动重连。
- **测试文件**：新增upload_delay_hook.cpp、upload_idle_test.py、windows_upload_idle_ssh_test.py及CMake专项；补application_pause_test与tcp_connection_test。覆盖慢同步前无ACK/无EOF、真正空闲仍关、正常上传、TCP RST丢失ACK后QUERY_DELTA缺块02→只补第二块→提交。独立静态审查未发现生产阻断问题；按审查建议用小SO_SNDBUF和512KiB输出保证错误排空测试连接仍打开。
- **Ubuntu实际命令/结果**：原工程GUI=OFF、复用缓存；`cmake -S . -B build`、`cmake --build build -j 2`、`ctest --test-dir build --output-on-failure`成功，最终生产代码 **51/51通过**（36.72秒）。本轮网络变更需要全量回归，实际重建此前清理的测试产物。随后只补RST及排空测试断言，重建application_pause目标并执行`ctest --test-dir build -R "application_pause_test|tcp_connection_test|upload_idle_integration" --output-on-failure`，**3/3通过**；无后续生产修改。
- **Windows实际命令/结果**：Qt6.8.3/MinGW13.1/Ninja/Debug；匹配Kit PATH下`cmake -S . -B build-windows-gui`、`cmake --build build-windows-gui -j 2`成功，`ctest --test-dir build-windows-gui --output-on-failure` **23/23通过**（117.35秒，非性能指标）。两项真实SSH脚本通过：新增慢持久化GUI双文件上传及SHA-256；既有windows_gui_ssh_test正常/增量/GUI进程kill+server SIGKILL重连续传、暂停取消与关窗恢复均通过。精确脚本参数与设计见 [R1_UPLOAD_DISCONNECT](R1_UPLOAD_DISCONNECT.md)。
- **失败记录**：GUI最初idle500ms导致两个文件都失败（第二请求也等数据库锁），不是预期单文件隔离，改1500ms后稳定复现一失败一成功；不改产品默认值。首轮修复后夹具ACK误用length导致KeyError，改协议真实data_length后通过；均未冒充通过。Windows既有Vulkan头告警非阻断。
- **版本/数据边界**：保留前轮未提交GUI与文档；Ubuntu同步前干净，仅同步本轮Linux源码、CMake及Linux测试，未同步全部Qt/文档，不声称三端全一致。使用脚本自建私有根，服务/隧道已停止，测试根保留；未覆盖真实备份，未提交/push。
- **剩余/未验证**：工作永久卡死仍可能占用有界任务/连接，没有新增服务端工作硬截止，客户端仍30秒等待上限；用户暂停/本地哈希导致真实网络空闲未在本轮修改。未跑Release/Sanitizer/TSan/MSVC/Linux GUI、默认30秒阈值长期慢磁盘、物理掉线/真实磁盘满/硬件掉电、GB文件/性能测试。受控慢同步和RST不等于硬件故障。
- **下一轮唯一建议**：若真实上传仍报断开，先用失败文件大小、暂停/哈希/等待时长与服务端参数关联日志，单独复现客户端真实空闲或其他关闭路径；不无证据增加心跳/重试。本轮到此停止，不自动继续模式或其他功能。

## R1 首项完成：GUI 可达性误判与周期检查（2026-11-03）

- **已完成**：真实MainWindow+loopback TCP红测确认“合法HELLO_ACK后健康对端关闭导致离线”误判；QtTcpTransport新增显式HealthState和healthChanged，顶部/总览显示未检查、检查中、可达、不可达，诊断文字不再驱动状态。用户首次检查后，每次独立HELLO完成15秒后复查、单次5秒超时；成功先标terminal再关闭socket，正常清理不覆盖结果。失败自动复查，手工立即重试；端点修改停止旧socket/超时/复查并失效结果。
- **范围与所有权**：仅R1这一项；socket/两个QTimer仍属Qt事件线程，QObject context管理回调，单实例一个在途探测；列表connectForRequests保持原请求模式，不启动健康复查，BackupWorker独占线程/socket不变。不改后端、协议、schema、持久化会话或备份数据。
- **文件**：QtTcpTransport.h/.cpp、MainWindow.cpp、DashboardPage.cpp；新增tests/unit/gui_health_test.cpp及CMake测试目标，gui_pages_test移除直接发射ready/failed伪造网络状态的旧断言。更新GUI_DESIGN、BUILD_WINDOWS、路线图状态；完整设计/学习点/命令与限制见 [R1_GUI_HEALTH](R1_GUI_HEALTH.md)。保留上轮所有未提交文档。
- **实际Windows验证**：Qt6.8.3/MinGW13.1/Ninja/Debug；`cmake -S . -B build-windows-gui`、`cmake --build build-windows-gui -j 2`均退出0；最终`ctest --test-dir build-windows-gui --output-on-failure` **23/23通过**（96.57秒，非性能指标）。健康测试44.95秒，使用真实15秒周期，覆盖peer关闭、应用沉默超时、周期恢复、端点取消旧超时、拒绝/手工恢复、在途销毁；原列表/worker/七类握手回归均通过。
- **独立无Qt验证**：原MinGW13.2缓存，`cmake -S . -B build-windows-protocol`、`cmake --build build-windows-protocol -j 2`、`ctest --test-dir build-windows-protocol --output-on-failure`均退出0，**16/16通过**。两套命令分别使用匹配Kit的PATH，见专项文档。
- **真实SSH**：本轮Windows GUI --probe-port经自建临时SSH隧道到Ubuntu原工程server的临时--handshake/随机端口（45563），退出0；按/proc/PID/exe确认归属后停止本轮服务和隧道，没有存储根和常驻服务重启。不据此宣称跨机周期故障测试通过。
- **失败与未验证**：旧实现先见行为断言红测；扩展测试两次因拒绝端口等待失败（首轮通用超时，定位后为refused connection），将测试2秒假设改为覆盖应用5秒截止的7秒容差，生产超时不变。Vulkan头和dxcompiler/dxil既有非阻断告警仍在。未执行Linux GUI/MSVC/Release/Sanitizer/长期检查、真实SSH断网/服务崩溃周期恢复、新GUI上传；未同步Ubuntu本轮源码/重跑Linux全量或重建此前清理的测试产物。
- **剩余问题/边界**：“可达”只表示最近HELLO成功，handshake模式也可达，不保证备份能力、磁盘/SQLite或提交健康；正常事件循环下故障约15秒复查+最多5秒等待后体现，睡眠/主线程停顿会延后。R1上传异常、备份模式及其他GUI问题尚未完成，R2以后未开始。
- **审查**：独立只读代码审查未发现阻断问题；重点核对terminal后关闭、双计时器取消、单次复查调度、请求/备份连接隔离及QObject生命周期，审查者未重复运行测试。文档链接、UTF-8、原M历史完整保留及git diff --check已核验。
- **下一轮唯一建议**：选择一个可复现的full/resume/incremental模式判断或上传异常，先写回归再最小修复；不自动执行。本轮到此停止，未提交或push。

## 当前状态：V3 路线冻结与现状审计（2026-11-03）

本节为最新状态；下方 M0–M7 记录按发生时保留，其中“未实现”“未提交”“下一步”等只代表当时，不覆盖本节。新路线见 [ROADMAP_V3](ROADMAP_V3.md)，功能矩阵、源码依据、差异与精确命令见[本次审计](PROJECT_AUDIT_2026-11-03.md)。

- **已完成**：本轮源码/文档/Git/环境审计和路线文档整理；既有 M0–M5 工程、Reactor、上传校验、SQLite持久续传、固定块增量/预览及真实Qt备份客户端已有实现与历史验收；M6只完成服务端当前文件分页、GUI文件分页和Linux CLI当前单文件安全下载，不代表M6全部完成。
- **进行中**：后续主线转为单用户可靠性；本轮仅建立文档与既有回归证据，尚未开始 R1 业务修复。
- **规划中**：R1连接/上传/模式/GUI回归；R2 GUI与文件夹恢复/服务端历史/指定版本/目录快照；R3 GB文件与容量；R4生命周期/数据库/日志/长期运行；R5认证隔离；R6管理/配额/审计；R7有界并行与FastCDC独立实验；R8压测/Docker/弱网及条件允许的云验证。依赖 R1→R2→R3→R4→R5→R6→R7→R8。
- **未实现边界**：GUI恢复、服务端历史查询与任意旧版本选择、目录快照、GB文件、完整版本GC/配额、TLS/认证/多用户/管理员、性能报告。已有不可变版本不是目录快照；下载 `--version-id` 仅检查当前版本，GUI版本页是本机提交记录。当前单文件仍限64MiB，仅可信loopback/SSH。
- **版本证据**：审计开始Windows工作分支 `codex/m5-gui`、Ubuntu原工程main均干净；两端HEAD及GitHub main只读查询均为 `31e5d3a7f2ea6235ba363cebc2001b06e0d08c79`。Windows本地main仍停M4，不自动切换/同步；本轮新增文档修改尚未提交，不据此宣称文档三端同步。

### 本轮真实验证

Windows Qt6.8.3 / MinGW13.1 / Ninja / Debug，复用现有缓存：

```powershell
$env:PATH='D:/QT/Tools/mingw1310_64/bin;D:/QT/6.8.3/mingw_64/bin;'+$env:PATH
cmake -S . -B build-windows-gui
cmake --build build-windows-gui -j 2
ctest --test-dir build-windows-gui --output-on-failure
```

配置/构建退出0，完整 **22/22通过**；56.84秒不是性能指标。Vulkan头及dxcompiler/dxil缺失告警非阻断，不作为正式打包验收。

Ubuntu原工程经 `ssh -o BatchMode=yes -o ConnectTimeout=8 miniob-ubuntu` 实际执行：

```bash
cd /home/ning/ChunkHarbor
cmake --build build --target chunkharbor-server chunkharbor-cli -j 2
python3 tests/integration/files_list_test.py ./build/chunkharbor-server ./build/chunkharbor-cli
python3 tests/integration/download_test.py ./build/chunkharbor-server ./build/chunkharbor-cli
```

构建及两项集成均退出0，覆盖真实上传/列表、当前下载、版本固定、安全发布与重启；使用脚本自建临时数据根。未执行Linux重新配置/全量CTest（保留此前按用户要求清理测试产物后的状态，未为本次文档审计重建全套测试）；50/50仅为历史。未重跑真实Windows GUI→SSH业务、Release/Sanitizer/TSan/MSVC/Linux GUI、GB端到端、长期/性能/硬件故障，不能宣称通过。

### 下一轮唯一最小任务建议

**R1：顶部连接检测状态的语义与窗口级回归。** 审查 MainWindow 的 statusChanged/ready、QtTcpTransport 探测后断开路径，与独立备份/列表连接的关系；先复现握手后断开、端点切换及独立请求结果造成的显示问题，再最小修复。现有探针主要检查握手退出码，不能证明顶部状态持续正确；本轮仅静态列出风险，未确认用户具体症状根因或修复业务代码。此项完成后再逐个排上传异常和模式判断，不直接跳入R2。

本轮改动仅为AGENTS、开发指南、README、BUILD_WINDOWS、PROGRESS及新增路线图/审计文档；未改协议/schema/持久会话、未触碰真实备份、未提交/推送。每轮按路线图末节执行学习说明→最小设计→复现/实现→实际验证→PROGRESS证据更新。

## 以下为原 M0–M7 开发历史（保留原文）

## M6发布与Ubuntu原工程同步完成（2026-11-02）

- 用户明确授权提交/推送GitHub并同步Ubuntu原项目；范围为已有M6分页、Linux下载、GUI文件列表及重扫/布局修复，不宣称M6全阶段完成。新增忽略`tests/testFiles`，不发布用户照片/ZIP、SDK、构建产物或备份数据。
- 功能提交`b6b62ab`已推送GitHub main，并通过Git bundle导入Ubuntu原项目后快进同步，不创建新源码目录。Ubuntu同步前的M6工作区保存于原仓库stash，242个文件归档内容按LF规范化SHA-256逐项核验，原M2归档保留。
- 本轮Windows原build构建成功、完整CTest22/22通过；Ubuntu原build仅重建server/CLI目标，文件列表和下载恢复两项真实集成测试通过，未重建此前清理的测试可执行文件。Linux50项全量、Release/Sanitizer及Windows真实SSH全流程未在本发布轮重跑，不以历史结果代替。
- 发布不包含tests/testFiles照片/ZIP或备份数据；未重启用户正在使用的服务，保留原build/deps和配置。上传模式含义已写入BUILD_WINDOWS.md。最终文档提交与三端一致性以Git核对结果为准。

## 中断重扫与最大化布局修复（2026-11-02）

- 日志和只读会话检查确认春熙路5个文件已COMMITTED，full重新预览仍要求目标不存在而报CONFLICT。仅对完整源/配置匹配的本任务completed记录允许Auto预览，远端须Unchanged，正式上传仍固定Exact基线；保留其他任务已有目标保护及未完成源变化保护，不删有效会话。未完成记录预检补充目的地前缀隔离。
- 预览卡片stretch=1而表格最大190/内部stretch=0造成标题统计拉高。删除表格最大高度，内部表格stretch=1，不改样式或其他组件。
- Windows实际构建成功，完整CTest22/22通过；原生Windows0/3/200行普通/最大化/还原/缩放几何测试通过。SSH原工程服务端专项覆盖A/B切换、取消/重扫/续传和SHA-256完整性，及源变化时保留旧会话、独立目的地重传。详细命令、失败记录及验证边界见BUGFIX_RESCAN_LAYOUT.md。
- 未改变协议/SQLite/持久化格式，不重构线程；未删除用户会话/备份，不提交或推送。未重跑Linux全量测试（本轮没有服务端修改，保留此前清理后的build状态）、Linux GUI/MSVC/Release/Sanitizer。

## Ubuntu build测试产物清理（2026-11-02）

- 按用户要求，仅清理`/home/ning/ChunkHarbor/build`中42个测试可执行文件/故障注入库、旧buffer-sanitized、CTest日志及M6同步临时包/日志，释放83.29MiB，build约195MiB。保留CMakeFiles、deps、构建缓存/静态库及server/CLI；未修改源码、测试源码或用户备份数据。
- 清理前后server、CLI、CMakeCache.txt、Makefile的SHA-256一致，server/CLI `--help`实际退出0；未重跑CTest以免重新生成已清理产物。删除清单在原仓库`.git/codex-build-cleanup.json`。CHUNKHARBOR_BUILD_TESTS仍开启，后续完整构建可能重新生成测试程序；再次运行CTest前须先重新构建。未提交/推送。

- 当前阶段：M5已完成；M6已完成服务端分页列表、Linux CLI下载恢复及Windows GUI文件页真实分页。GUI恢复、并发资源专项和性能实验尚未完成。
- 最近更新：2026-11-02
- 本轮范围：按用户要求将最新M6源码同步到Ubuntu原工程并复用原build构建验证，不新增源码工程目录，不提交或推送。

## M6同步Ubuntu原工程（2026-11-02）

- 已将Windows最新未提交M6源码同步到`/home/ning/ChunkHarbor`，复用`build`和既有SQLite/OpenSSL依赖。同步前两端HEAD相同、Ubuntu工作区干净；不修改Git历史，保留原stash。242个源码文件逐项SHA-256一致，审计清单在原`.git`内。
- 排除Windows `tests/testFiles`的照片/ZIP及构建、SDK、备份数据；未修改`/home/ning/exchangeFiles`数据。未创建新源码工程目录，未启动常驻服务。
- Ubuntu原工程实际Debug配置/构建成功，完整CTest **50/50通过**（30.57秒，非性能指标），覆盖M6文件列表及下载。命令、日志和同步方式见UBUNTU_SYNC.md；Linux CLI help包含download。
- 此后日常使用`/home/ning/ChunkHarbor/build/chunkharbor-server`与`chunkharbor-cli`，不再要求/tmp测试构建。同步后的M6仍为未提交改动，不代表已发布GitHub。本轮未重跑Windows/GUI、Release或Sanitizer。

## M6 Windows GUI服务端文件列表（2026-11-02）

- FilesPage改为服务端当前已提交文件，新增ServerFilesModel（QAbstractTableModel）和FileListController；版本历史页仍为本机记录。当前页最多32条，显示逻辑路径、精确字节数、版本和SHA-256；搜索明确仅筛选当前页，不伪造本地路径/时间/服务器总数。
- QtTcpTransport增加独立请求模式，复用HELLO与file_list_v1；每页新连接、每次仅一个在途请求，保持所属Qt线程异步socket/timer，无阻塞waitFor/扫描或线程等待。5秒握手、10秒响应超时、64KiB控制接收上限、request_id/类型关联及跨游标检查；原连接检测模式和顶部状态含义保留。
- 刷新/上一页/下一页/取消可用；最多128个历史游标，不累计文件全集。失败保留原页和游标并标记上次结果，修改端口清空旧结果、取消在途查询；关窗按QObject所有权销毁。恢复按钮禁用。详情使用只读可滚动QPlainTextEdit，tooltip整体转义后添加受控标签，远端路径不解释为HTML。
- 行为红测先确认旧文件页错误地由本机提交填充；实现后Windows Qt6.8.3/MinGW13.1/Ninja构建成功，最终完整CTest **21/21通过**（新增gui_files_test覆盖21类场景，原七类握手及备份worker回归保留）。
- Ubuntu隔离源码`/tmp/chunkharbor-m6-gui-list-SmsSMQdt`实际GUI=OFF配置/构建成功，完整 **50/50通过**；本轮未改后端协议和存储逻辑。最终Windows原生Qt窗口→SSH→专用Ubuntu根`/tmp/chunkharbor-m6-gui-files-data-UKda26Zy`联调通过：真实CLI提交71文件，首32/次32/返回首32、中文筛选、大小和SHA元数据逐项比对通过。服务与隧道已停止，专用数据根保留供检查。
- 原生Windows截图检查使用正式qrc/Fusion浅色palette，中文可读，详情多行布局改为滚动文本避免压缩裁切；build/m6-gui-files.png为忽略的测试截图，不上传。独立静态审查发现tooltip富文本问题后修复，复审无剩余Critical/Important；审查者未重跑测试。
- 失败记录：close专项最初因Qt最后窗口关闭自动退出，返回0但探针没输出；设置测试app不自动退出后通过。offscreen截图缺字形，切到原生Windows平台复核；原生探针输出在4096字节缓冲边界截断，显式flush后通过。详情原生高度检查曾失败，改为只读滚动文本后通过。这些失败未计为成功。
- 未运行Linux GUI/MSVC/Release/Sanitizer、GUI下载、全新GUI上传端到端及性能/大规模并发；分页是实时视图而非快照，当前仍仅可信loopback/SSH，无TLS/认证。未更新Ubuntu主工程、未触碰用户数据；未提交/推送。
- 文件映射/设计见M6_GUI_FILES.md，命令与结果见M6_GUI_FILES_VALIDATION.md。下一最小任务：GUI单文件恢复控制器与Qt下载传输适配，复用现有下载协议。

## M6 Linux单文件下载恢复（2026-11-02）

- 新增download_v1能力，13 BEGIN_DOWNLOAD、32 DOWNLOAD_READY、33 GET_DOWNLOAD_CHUNK及14 DOWNLOAD_CHUNK；二进制块复用显式大端DATA_CHUNK布局，前16字节表示version_id。新增32/33只在协商后使用，旧客户端无需改变，旧服务端不支持新下载能力。
- 仅选择当前已提交版本，可用version_id做当前版本前置条件；固定已打开FD保证后续新版本发布不混读。兼容旧upload COMMITTED原文件，不扫描全库或触发legacy迁移；首次整文件核验与每次有界块读取/哈希均在DiskExecutor。每连接一个下载会话、每请求一块最多256KiB，默认文件上限仍为64MiB；不据此宣称已完成M6并发资源压测。
- Linux新增`download --path LOGICAL --output LOCAL [--version-id ID] [--port N]`。本地父目录逐组件O_NOFOLLOW固定目录FD；随机O_EXCL/0600临时文件；严格校验响应身份/范围/块哈希与整文件SHA-256；fsync临时文件→renameat2(RENAME_NOREPLACE)→父目录fsync。正常错误RAII清理临时文件，已有目标和发布竞争均拒绝覆盖。目录fsync失败时文件可能已发布，返回失败且保留文件；强制终止可能留隐藏临时文件，不是下载续传。
- 验证：旧服务端先按预期因缺download_v1红测。Windows Qt6.8.3/MinGW13.1/Ninja实际构建成功，完整CTest **20/20通过**；Ubuntu SSH隔离源码`/tmp/chunkharbor-m6-download-zjetPmKD` Debug配置/构建成功，最终完整CTest **50/50通过**。命令和边界见M6_DOWNLOAD_VALIDATION.md。
- 真实集成覆盖空/中文/多块二进制、旧upload即时下载、增量版本、发布期间旧FD固定、版本前置条件、重启、已有目标/父目录链接、服务端对象损坏/符号链接/FIFO及非法请求。恶意peer覆盖17类场景，包括已写入一块后断连、整文件/块校验、错误身份与发布竞争；四类LD_PRELOAD注入覆盖ENOSPC、文件/目录fsync、短写+EINTR。不是物理磁盘满或硬件掉电测试。
- 失败记录：首轮Ubuntu编译将SqliteStore包装器误传给sqlite3语句构造，改为nativeHandle后重建通过；一次恶意peer夹具发送超限控制帧导致正确提前拒绝与测试端BrokenPipe，改为合法长度但错误类型的数据帧后全量通过。独立只读静态审查未发现Critical/Important问题，审查者未独立重跑测试。
- 未执行Release/Sanitizer/MSVC/Linux GUI、本轮Windows GUI真实上传端到端、GUI下载、下载续传、性能或大规模并发实验；仍限可信loopback/SSH、无认证。没有更新Ubuntu主工程或真实备份数据；未提交/推送。
- 下一最小任务：Windows GUI文件页接入服务端分页列表及后台任务模型，再接同一下载协议的恢复控制器。

## M6受限分页文件列表（2026-11-02）

- 新增纯C++17 FilesList协议，严格校验字段、重复键、UTF-8路径、整数、顺序及游标；大小以uint64整数处理。默认32条，最多64条，负载最多64KiB，按路径字节序keyset分页；并发变化下是实时视图而非跨页快照。
- SqliteStore以有界SELECT合并current_files与旧upload的COMMITTED记录，当前版本优先，排除未提交/preview/历史版本；新增幂等COMMITTED路径部分索引，兼容schema v2、不触发数据迁移或正文哈希。EXPLAIN QUERY PLAN实际显示两分支路径索引SEARCH及MERGE，无OFFSET/临时排序。
- UploadHandler独立协商file_list_v1，沿用有界DiskExecutor、pause/resume及weak_ptr完成回调；不改变上传会话绑定。数据库锁仅覆盖查询，有界值对象在解锁后编码，连接I/O仍归所属loop。
- TDD：旧Ubuntu服务端真实TCP专项先因未协商file_list_v1断言失败；实现后新增协议/集成测试通过。首轮编译发现DeltaJson辅助头依赖间接包含，补齐DeltaControl/UploadControl头后双平台重新构建成功；失败不计作通过。
- Windows Qt6.8.3/MinGW13.1/Ninja：`cmake --build build-windows-gui -j 2`成功；`ctest --test-dir build-windows-gui --output-on-failure`完整 **19/19通过**。
- Ubuntu SSH隔离源码`/tmp/chunkharbor-m6-list-KhFt0dZP`：Debug配置/构建成功，最终完整CTest **46/46通过**。专项涵盖空库、旧upload即时可见且查询不迁移、当前版本去重、未提交/preview不可见、中文排序、64条截页、长路径JSON转义64KiB截页、同连接上传/列表互不干扰、非法输入、缺少握手/能力及重启分页。
- 未运行Release/ASan/UBSan/TSan、Linux GUI/MSVC、本轮Windows GUI真实上传端到端或下载测试；未做吞吐/内存峰值实验。未改Ubuntu主工程或真实备份数据，默认仍仅loopback/SSH、无认证。
- 协议及文件映射见M6_FILES_LIST.md；实际命令与验证边界见M6_VALIDATION.md。下一最小任务：BEGIN_DOWNLOAD/DOWNLOAD_CHUNK协议与Linux CLI单文件临时下载、SHA-256校验发布闭环。

## Ubuntu主工程同步完成（2026-11-02）

- 展开74项旧状态为82文件：79个与历史版本一致，其余仅README路径/旧进度记录/尾空格，无独有功能需移植。完整include-untracked stash并逐文件核对SHA256后，快进同步M5；保留主项目内部stash与审计JSON，不生成新的外部备份文件夹。
- GitHub对象经Windows既有代理读取、bundle/SSH导入Ubuntu，origin URL不变，未强制reset。`/home/ning/ChunkHarbor`完整代码含GUI，GUI=OFF无需Qt；SQLite/OpenSSL沿用主项目build/deps。
- 本轮Ubuntu主工程实际配置/构建成功，Debug44/44通过；工作区干净。同步细节与复现命令见UBUNTU_SYNC.md。未重跑Windows/GUI/Release/Sanitizer，终端GBK解码异常经独立Git核查确认未影响已完成的远端操作。

## M5发布整理（2026-11-02）

- 用户明确授权上传GitHub。发布包括中文六页GUI、真实扫描/预览/上传/续传、任务模型、本机历史/日志/设置、统一下拉框与独立百分比、相关资源/测试/文档，排除构建目录、临时截图/探针、SDK、备份数据。删除不再参与构建的旧DemoData示例文件。
- GitHub直连443失败，使用Windows系统已配置的127.0.0.1:7897代理成功读取远端，未修改全局Git代理配置。发布前远端main为db09608（M4）。
- README补充同仓跨平台策略：Ubuntu同步完整源码但GUI=OFF，无需Qt；构建及备份数据不进入Git。Ubuntu /home/ning/ChunkHarbor当前有74项本地改动，版本仍为早期M1提交；本轮不对其pull/reset或覆盖，不能宣称Ubuntu已同步发布提交。
- Windows发布前实际执行`cmake --build build-windows-gui -j 2`成功（无待编译项），完整CTest18/18通过；暂存48个必要文件，路径/明显凭据与构建产物扫描及diff检查通过。Git提交/推送以本轮工具输出为准；此前Ubuntu44/44、真实SSH及样式截图属于历史验收，不作为本轮重跑。

## GUI下拉列表与百分比显示修复（2026-11-02）

- 根因：QComboBox弹出列表仍使用平台菜单delegate，缺少统一的列表项/箭头/边框样式；QProgressBar被QSS限制为8px高度却仍绘制百分比文本，造成0%等文字裁切。
- 新增widgets/ComboBox.h供Backup/Files/Logs/Settings全部下拉框共用，使用QListView和QStyledItemDelegate、统一列表间距及无系统框线的弹出容器；Light.qss统一圆角白底、浅蓝选中/悬停/焦点态，资源内新增矢量chevron-down.svg。
- BackupPage隐藏细进度条内置文本，将百分比放到上方独立QLabel，未知总量显示破折号；实际确认进度与任务结束信号继续更新0–100%。控件和delegate沿用Qt父子所有权，无网络/工作线程或协议修改。
- Windows Qt6.8.3/MinGW13.1：`cmake --build build-windows-gui -j 2`成功，`ctest --test-dir build-windows-gui --output-on-failure`18/18通过。使用隔离QSettings/临时数据目录的截图工具检查文件/日志/备份/设置下拉框及0/50/100%布局，系统125%及QT_SCALE_FACTOR=1.6（合计200%）下数值标签高度不小于字体高度，进度条不再绘制文字。图像使用与正式main相同的浅色palette并等待重绘；首次辅助截图未设置palette出现暗色容器，修正截图环境后复查正常。
- 未执行Linux GUI/MSVC或新的真实上传联调；本轮仅样式和显示布局修复，原M5验收作为历史结果。产品构建保留用户当前CLion窗口未关闭；用户需关闭旧窗口后重新构建运行。未提交/推送。

## 旧备份与验证目录清理（2026-11-02）

- 用户明确要求ChunkHarbor相关仅保留主项目目录。核对两个bundle的HEAD均存在Windows主项目Git历史，旧m3-verify仅含SQLite开发包，没有额外源码或用户数据。
- 先将SQLite完整复制至`/home/ning/ChunkHarbor/build/deps/sqlite`，保留包内符号链接并逐项核对15个条目的类型、链接目标与文件SHA256；`c++ -x c++ -fsyntax-only`实际包含sqlite3.h检查通过。未覆盖主项目已有文件。
- 校验后删除`/home/ning/ChunkHarbor-buffer-base.bundle`、`ChunkHarbor-m1.bundle`、`ChunkHarbor-before-buffer-20261030.tar.gz`、`ChunkHarbor-m3-verify`。事后枚举/home/ning的ChunkHarbor相关项仅剩`ChunkHarbor`；MiniLogDB及系统/个人目录保留。
- 主项目现有CMake缓存未发现旧SQLite目录引用。历史验收文档中的旧路径属于当时执行证据，保留原文；未来配置使用BUILD_WINDOWS.md注明的新依赖路径。本轮未重跑产品全量测试，未改产品源码、未提交/推送。

## 测试产物清理（2026-11-02）

- 用户报告手工测试结束并明确要求删除测试项目/文件。先核对目录内容、历史记录、服务进程、绝对路径与链接，再清理；未泛化删除用户目录或其他项目。
- Ubuntu：正常停止已核验PID4817的测试服务；删除`ChunkHarbor-verify-20261030`（干净的旧M1测试副本）、`chunkharbor-m3-accept-h4oJyukB`、`ChunkHarbor-gui-test-local-BHyJwFs2`及/tmp下三个chunkharbor-gui-manual测试数据根。`ChunkHarbor-m3-verify`测试源码和build/build-asan/build-release已清理，仅保留仍被构建文档引用的`deps`（SQLite开发依赖）。正式ChunkHarbor、MiniLogDB及Git bundle/tar备份保留，事后检查通过。
- Windows：正常关闭测试GUI；已将专用源目录、M3验收源、四个M5自动验收目录、review-begin-upload目录共七个目录，以及三个临时探针/截图文件、两个失效远程路径记录文件移入回收站。事后检查匹配的Temp测试目录已不存在；正式源码和cmake-build-windows-gui可执行文件仍存在。QSettings中已清除指向本次测试源目录的失效directory值。
- 限制：Windows批量永久删除被自动安全审核拒绝（仅返回blocked by policy），改为可恢复回收站后上述测试文件清理成功。`%LOCALAPPDATA%/ChunkHarbor/chunkharbor-gui`测试历史/日志/会话目录回收时出现UnauthorizedAccess，保留原ACL，不绕过权限；该目录仍需用户在资源管理器手动删除，已在交付中明确说明。
- 本轮仅清理与文档更新，未运行产品编译/功能测试，不改变M5历史验收结果，不提交/推送。

## 手工上传测试准备：GitHub连接失败（2026-11-02）

- 用户git clone遇到github.com:443连接被拒绝。未推断具体代理/防火墙原因，保留原Ubuntu工程及修改，不调整网络配置。
- 通过Windows本地源码打包并经现有SSH传到新建目录`/home/ning/ChunkHarbor-gui-test-local-BHyJwFs2`，仅包含源码/头/构建/测试/第三方文件，不包含.git、用户数据或构建产物。
- 新目录Debug服务端+CLI配置/构建成功，GUI/TESTS关闭，复用既有SQLite/OpenSSL开发头和系统库，不安装系统软件。`chunkharbor-server --help`成功；独立临时数据根实际CLI preview返回Added/0发送块，backup返回BACKUP OK/1块，原文件和正式文件一致，SHA256为ab321384f5765706e9117c1e145313791a7030f8fae12ad8e43069fde15d22d3。
- 冒烟服务已停止；8787由用户按手册启动。未重跑全量CTest或Windows GUI端到端，不将此前M5完整验收当作本轮重跑。Windows源码未修改，保留原工作区，不提交/推送。

## M5真实GUI集成与验收（2026-11-02）

- TransferController + TaskModel + BackupWorker接入实际目录、源快照、服务端分页差异计划和runDelta上传。后台独占Socket/文件句柄，queued回GUI；安全暂停/取消、关窗协作退出、丢失提交回执恢复、配置/摘要/基线绑定、真实ACK进度和10000条目上限。
- 六页真实数据：总览状态联动；备份预览/上传；文件/版本显示本机成功提交记录；日志持久化/筛选/导出；QSettings保存端口、块大小、续传、排除、启动页和日志目录。恢复/认证留给M6/M7并禁用；并行1、SHA256、浅色为固定能力。
- Windows Qt6.8.3/MinGW13.1/Ninja构建成功，完整CTest18/18通过；Ubuntu独立目录/tmp/chunkharbor-m5-HZIF3EXX当前CMake GUI=OFF配置/构建，Debug44/44通过。真实Windows Widgets/Controller经SSH到Ubuntu：首次/0块/1块、中文名/空文件/排除、源变化/外部基线保护、Windows进程kill+服务端SIGKILL恢复、暂停取消与Resume、优雅关窗重开，均通过且SHA256一致。
- 六页截图检查后调整备份布局，默认窗口完整显示任务操作。Fake TCP覆盖严格request_id、恶意plan session/base、full→resume、COMMITTED本地回执丢失、状态写失败/源消失/取消；本机记录UTF8和64位精度、设置及页面联动有回归。
- 真实联调暴露request_id回退，修复独立连接后通过；另修复可信计划绑定、模式继承、进度分母、端点在线状态、非法1MiB块选项、条目上限和probe设置污染。并发exe占用、上限测试无效配置等失败已记录。最终独立审阅未发现新Important问题。
- 线程/文件映射见GUI_DESIGN.md，命令结果见M5_VALIDATION.md，CLion/SSH和专用目录手工指令见BUILD_WINDOWS.md。Linux GUI/MSVC/新增GUI Sanitizer/Release及性能实验未执行。
- 保留前轮未提交UI，分支codex/m5-gui，未提交/推送。下一最小任务：M6受限分页文件列表及恢复协议，先完成单文件临时下载+SHA256校验发布闭环。

## GUI完成度审查（开发前快照，2026-11-02）

- 已核对页面、MainWindow、QtTcpTransport、DemoData、GUI测试和开发指南：六页框架、中文样式、目录选择、示例筛选/导出及模拟进度已实现；真实业务仅接入HELLO握手。M5尚未完成。
- 纯界面缺项：FilesPage仍为占位，缺少搜索/筛选/文件表格/详情；Settings仅有loopback地址、端口和检查连接，备份参数、外观/启动页/日志目录及设置持久化未实现。地址只读是当前SSH隧道开发约束，不作为缺陷。
- Dashboard统计、最近任务及活动仍为空状态；连接卡片固定为“尚未检查”，没有订阅顶部的真实连接状态，存在跨页面状态不一致。
- Backup使用DemoData和QTimer，没有真实扫描/哈希/差异计划/上传、真实速度与确认进度、暂停取消重试、持久会话恢复；Versions使用固定种子摘要和示例历史，恢复禁用；Logs筛选/导出真实可操作，但来源仍是五条内存示例日志。
- 集成缺项：TransferController、真实任务/预览模型、后台扫描与哈希、Qt线程适配、错误和进度事件、关闭/重开续传及GUI到Ubuntu端到端验收。client_core已有DirectoryScanner、SourceSnapshot与同步runDelta，可复用；同步驱动不能直接放在GUI线程，也不能让工作线程直接操作GUI线程的QTcpSocket。
- 文件/历史浏览还需服务端分页查询协议与实现，不能仅将示例数据换成数据库读取。下载恢复属M6；TLS/认证、正式打包部署属M7；当前已有windeployqt开发依赖部署，不等于M7完成。定时备份、版本保留/GC等仍为后续P1功能。
- 本轮Windows Qt6.8.3/MinGW13.1实际执行：`cmake --build build-windows-gui -j 2`成功（ninja无待编译项）；`ctest --test-dir build-windows-gui --output-on-failure`，16/16通过。仅证明现有演示/握手及共享模块回归通过；本轮未运行Ubuntu、GUI真实上传/续传/恢复、MSVC、Linux GUI或性能实验。
- 下一最小任务建议：先接入后台真实目录扫描及预览模型，以专用测试目录验证中文路径、排除项、错误与取消；随后接入服务端差异预览与真实备份。首页连接卡片可单独修复。当前GUI相关改动仍有未提交文件。

## GUI简体中文（2026-11-01）

- 六页导航/标题/按钮/表头/提示/空状态/无障碍名称及示例数据、日志导出内容、模拟进度、连接状态和错误已中文化；保留品牌、协议及稳定命令行参数。模型判断和测试同步更新为中文，避免翻译破坏筛选/状态配色。
- 设置中文QLocale，当前Qt Kit的qtbase_zh_CN.qm通过资源嵌入加载，支持Qt标准控件中文；样式优先微软雅黑UI。MSVC目标增加UTF-8源编码选项，其他平台不引入新依赖。
- 实际Windows Qt6.8.3/MinGW13.1：build-windows-gui构建、完整CTest16/16通过，涵盖中文演示流程/筛选/版本选择与七类握手场景；六页截图检查。Linux GUI/MSVC未验证，不宣称已完成真实GUI备份。详细说明见GUI_DESIGN.md。未提交/推送。

## M5第二轮静态UI（2026-11-01）

- 新增集中DemoData及TableKit，细化三个页面和InfoCard副标题；QStandardItemModel承载示例数据。Backup具备配置、四类变化预览和完整模拟阶段/暂停/取消/重试；Versions双栏和详情联动；Logs组合过滤及用户触发的原子示例导出。
- 所有示例明确标注Demo/Sample，不发起备份、不扫描目录、不更改顶部真实连接状态。恢复按钮禁用。定时器与模型由Qt父对象持有，MainWindow没有增加业务处理。
- Windows Qt6.8.3/MinGW13.1：实际build-windows-gui配置自动重生成、构建、完整CTest最终16/16通过；新增页面行为检查，原有握手七场景回归。三页真实截图检查后压缩Backup卡片内边距，使默认窗口完整显示进度区；小窗口仍可滚动。
- 编写脚本初次读取UTF-8文件使用默认GBK而中断，显式UTF-8后完成；新增计时回归初稿断言顺序错误导致一次测试失败，调整顺序后全量通过，没有将失败记为成功。
- 文档及文件映射见docs/GUI_DESIGN.md；未执行Linux GUI/MSVC/GUI备份端到端。没有提交/推送。下一最小任务建议：先确定真实任务模型和TransferController与现有client_core的线程适配，再接入扫描/预览。

## M5第一轮UI骨架（2026-11-01）

- 运行依赖修复：新增默认OFF的Windows GUI `CHUNKHARBOR_DEPLOY_GUI`开关，开启后POST_BUILD调用当前Qt的windeployqt复制DLL/插件/编译器运行库。已配置开启、构建成功，Qt GUI CTest15/15通过；清理Qt环境变量且PATH只保留Windows/System32、独立临时cwd启动截图退出0。Linux/MSVC部署未验证，不代表正式打包。用户在CLion CMake选项加`-DCHUNKHARBOR_DEPLOY_GUI=ON`后重新构建即可。未提交/推送。
- 首次部署后headless握手测试因未复制qoffscreen插件超时；BUILD_TESTS启用时补充复制该测试插件后，完整15/15通过。隔离运行探针首次取环境变量时因SYSTEMROOT大小写失败，修正探针后实际启动退出0且截图存在；此前失败不计成功。

- 按用户明确范围实现MainWindow布局、图标+文字侧栏、顶部连接状态/地址/刷新/设置、六个独立页面、WorkspacePage及InfoCard、资源打包Light.qss。Dashboard仅空状态卡片，其余业务页保留布局空壳。
- 保留QtTcpTransport和probe-port真实异步握手；普通启动Offline，不自动伪造Online或备份数据。所有控件由Qt父子生命周期管理，未新增工作线程/阻塞I/O。
- 实际Windows Qt6.8.3/MinGW13.1/Ninja：cmake --build build-windows-gui -j 2，CTest 15/15通过（gui_probe含7握手场景）；Dashboard/Settings截图检查及其余页面启动检查。首轮截图发现Windows暗色palette透入内容区，显式浅色palette和StyledBackground修复后重新构建验证。
- 文件结构和命令见docs/GUI_DESIGN.md。未执行Linux GUI/MSVC/GUI备份端到端；没有宣称M5全部完成。未提交/推送。
- 下一最小任务：Backup页配置/扫描预览/任务进度的静态UI与数据模型设计，再逐步接真实核心。

## M4全阶段完成与最终验证（2026-11-01）

- 已实现：分页manifest/plan、新能力协商、可移植差异算法、schema v2、不可变版本及旧会话映射、持久清单和缺失块、块复用/确认、条件发布/崩溃恢复、双平台CLI backup/preview、源快照核验、目录扫描/取消/重试/汇总。所有阶段仍使用既有Reactor和有界工作池。
- Ubuntu新建源码隔离目录`/tmp/chunkharbor-m4-full-umBcFYdE`：最终Debug配置/构建/CTest **44/44通过**，Release **44/44通过**，ASan/UBSan **42/42通过**（两个LD_PRELOAD测试不与sanitizer组合）。最终完整测试包含FIFO拒绝、换块大小不污染旧版本、SQLite确认失败不ACK和新增错误注入。
- Windows Qt6.8.3/MinGW13.1/Ninja：独立`build-windows-m4`配置/构建/CTest **17/17通过**。`python tests/integration/windows_delta_ssh_test.py build-windows-m4/chunkharbor-cli.exe miniob-ubuntu /tmp/chunkharbor-m4-full-umBcFYdE/build/chunkharbor-server`真实通过：全量/0块/1块、预览、中文目录、Windows客户端中断及Ubuntu SIGKILL后同源续传、最终SHA一致。专用测试根`/tmp/chunkharbor-m4-win-ll0a8yan`保留供检查。
- 真实恢复：Linux原生rename屏障及四个LD_PRELOAD屏障精确暂停后SIGKILL；Linux CLI和目录批任务真实SIGINT后恢复、跳过已完成文件。错误注入覆盖ENOSPC、fdatasync、对象fsync、发布目录fsync及SQLite确认触发器；不等同硬件掉电/真实磁盘满。
- 审阅及修复：发布临时文件移入内部.delta避免碰撞用户合法.publish-ID；缩短/截空预览用明确change_kind而非零缺失块推断；已完成preview释放活跃槽并有逻辑元数据配额；不变内容换块大小不修改旧version_chunks；新Linux源文件open使用O_NONBLOCK避免FIFO阻塞。关键审阅问题已回归，最终P2修复经独立复审确认。
- 此前发现的M3 COMMITTED/EXPIRED清理缺meta启动失败已在兼容路径修复；旧会话读取不可变历史对象，最新副本另行校验。旧M3恢复/协议回归通过；未对用户真实备份目录执行升级。
- Ruling：存储实现集中于DeltaStorage内部、通过UploadStorage共享根锁/FD/SQL所有权，未按原建议拆成独立VersionStore/DeltaSessionStore，以避免双锁与双份路径安全代码；新增网络状态接入既有UploadHandler以共享握手/背压。对应功能及错误路径仍逐项测试。
- Ruling：PlanDone新增严格change_kind（Added/Modified/Unchanged），由基线大小及整文件摘要判断；预览逻辑元数据64MiB、总20000会话，只有COLLECTING占8活跃名额；串行目录待处理队列实际仅1项，小于128上限。
- 过程边界：部分并行模块先实现后补测试，不能宣称全部严格TDD；manifest/FrameCodec/planner消费边界、预览契约、FIFO、版本几何等确有红绿记录。Windows曾并行构建同目录导致对象损坏，后改独立目录单构建通过，失败未计为成功。
- 具体命令、结果及详细人工复现见`docs/M4_VALIDATION.md`、`docs/M4_USAGE.md`。本轮无需用户先人工测试才能继续；可按文档复现并反馈结果。未运行TSan、物理掉电/磁盘满、性能基准、GUI上传或打包；内部历史版本无自动GC和正式总空间配额。未提交/推送。

以下各节为前轮设计、M3/M2历史记录；“未实现/未修复”等状态以本节及源码为准。

## M4任务1实现与验证（2026-11-01）

- 新增ManifestPage.h/.cpp和manifest_page_test，注册既有protocol及CTest；四字段严格JSON、512摘要/128页边界、最大控制正文、hex/整数/NUL/嵌套校验；可信文件几何和页序单独验证。
- 返回对象拥有摘要数据，不引入Qt/POSIX/OpenSSL依赖或持久状态；没有修改服务端业务、旧消息及能力协商。新增单测包含完整帧golden、拆包粘包、末页、65536块、错误会话、数组/整数超限及文本边界。
- TDD：可编译空实现的新增测试先失败（manifest assertion failed），实现后Windows构建及全量CTest **10/10通过**。Ubuntu新建隔离目录`/tmp/chunkharbor-m4-aaEMsbwA`配置/构建/全量CTest **31/31通过**；补充测试后Windows全量10/10、Ubuntu重新构建及专项1/1通过。命令、依赖路径和边界见`docs/MANIFEST_PAGE.md`。
- 本轮未执行ASan/Release/TSan/GUI或实际增量上传；暂不需要用户人工测试。未提交/推送。
- 独立只读审阅未发现阻断问题，指出hex长度/类型等独立回归缺测；已补齐会话ID/摘要长度和非法hex、空数组、ID错误类型用例。补充后Windows重新构建/全量10/10、Ubuntu重新构建/专项1/1通过；无实现修改。

## M4 启动：MANIFEST_PAGE设计（2026-11-01）

- 用户明确确认M3正确性已验证并要求进入M4；不重做M3验收，也不把已知清理恢复问题作为本轮纯协议设计的前置任务。该问题并未修复，后续可靠性验证仍需跟踪。
- 草案见 `docs/superpowers/specs/2026-11-01-m4-manifest-page-design.md`：type18，session_id/page_index/first_block_index/hashes四字段；每页最多512个SHA-256，单页及可信文件上下文分层校验；纯C++17，不依赖Qt/POSIX/OpenSSL。
- 本轮仅设计，不修改源码、CMake或测试；不广告增量能力，不声称无变化0 DATA_CHUNK已可运行。基线版本、MANIFEST_DONE、分页计划、持久化复制及目录预览为后续独立子任务。
- 现有衔接限制：BEGIN_UPLOAD拒绝现存目标，旧UPLOAD_PLAN最多512区间；未来增量会话入口和版本保护需单独设计，不能绕过现有冲突检查。
- 首个实现子任务及测试矩阵已写入草案；设计确认后按TDD实现ManifestPage编解码和上下文校验。
- 独立只读设计审阅已完成，未发现阻碍首个编解码子任务制定实施计划的重要遗漏。Python按最大全页（512摘要、页127、起始65024）计算紧凑JSON为34407字节，小于65536字节上限；这只是设计尺寸检查，不是新增C++功能测试。
- 本轮Windows执行 `ctest --test-dir build-windows-m3 --output-on-failure`，现有测试 **9/9通过**；`git diff --check`通过。仅改文档，未重新配置/编译，也未执行Linux、ASan/Release、GUI或新增协议测试；这些结果不能标为本轮通过。未提交/推送。

## M4 完整开发计划（2026-11-01）

- 按用户要求新增 `docs/superpowers/plans/2026-11-01-m4-development-plan.md`，覆盖13个顺序子任务：manifest模块、其余协议、纯差异算法、v2 schema、不可变版本/旧数据兼容、持久清单、差异计划与复制、条件发布恢复、服务端状态机、源快照核心、双平台CLI、只读预览、目录批任务及总验收。任务13再分扫描、接入、验收三轮。
- 计划明确先单文件增量链路，再续传验证、预览和目录。每项单独测试/审阅，不表示一次授权实现全部M4；本轮未实现任何计划中的接口。
- 版本对象与最新副本分开，CAS保护目标；预览复用同一算法但不产生上传确认。此为建议设计，需要在相应子任务契约/测试中固定；不宣称已完成schema迁移、版本存储或增量上传。
- 计划自审覆盖指南M4要求、接口依赖、资源上限、旧M3兼容、失败与恢复测试；修正PLAN_READY到TRANSFERRING的衔接。M3已知清理问题纳入任务5兼容/恢复回归，不重做已有人工验收。
- 本轮实际检查：`git status --short`、相关约束/源码阅读、`git diff --check`；只编写文档，无需产品构建，未运行CMake/CTest/Linux/Windows功能测试。上一节9/9为此前设计轮结果，不能计作本轮重跑。未提交/推送。

## M3 当前实现与边界（2026-11-01）

- 服务端：新会话在暂存目录同步后写 SQLite；块在哈希校验、数据/回执同步和 SQLite 块事务提交后 ACK。断连后会话按 ID 查询，`SESSION_STATUS` 用最多8192字节位图表示65536块进度，避免区间数上限阻断合法续传。
- 启动时核对暂存元数据、数据库参数及每个已确认块的实际 SHA-256；损坏块从确认集移除。`COMMITTING` 若目标已发布则重新校验整文件并协调 SQLite；已提交目标丢失或哈希不符时拒绝启动。无 DB 记录的暂存目录隔离至 `.quarantine`；有 DB 记录但暂存目录缺失标为 `FAILED`。30天未更新的 CREATED/RECEIVING 会话启动时标为 EXPIRED 并清理其受限暂存文件。
- `COMMITTING` 尚未发布且暂存块损坏时，启动核验移除坏块并退回 `RECEIVING`，避免客户端补传被状态机拒绝；相应回归用例已在隔离目录运行。
- Windows Qt CLI：`--state` 指向原子写入的本地会话文件；重新打开源文件并计算 SHA-256 后才能续传。`status --state` 查询保存进度，`status --session-id` 查询历史 ID；再次运行 `upload` 仅发送缺失块，最终核对 COMMIT_RESULT 与源文件摘要。服务端仅监听 loopback，Windows 通过 SSH 本地转发连接。
- 自动测试覆盖 Linux Debug、ASan/UBSan、Release 与 Windows MinGW/Qt；最新精确结果与人工验收输出见 `docs/M3_VALIDATION.md`。M3 源码、测试与文档在同一发布提交中同步。
- 最新验证：Ubuntu稳定隔离目录 `/home/ning/ChunkHarbor-m3-verify` Debug 30/30、ASan/UBSan 30/30、Release 30/30；Windows Qt CLI 9/9、GUI原型9/9；Windows CLI经SSH隧道真实上传、状态查询及仅补缺失块测试通过。Debug、ASan/Release完整回归均包含最新暂存清理改动。具体命令、人工步骤见 `docs/M3_VALIDATION.md`。
- 用户人工验收（2026-11-01）：32MiB随机源文件、65536字节分块（512块）；Windows CLI中断前终端显示8行ACK，Ubuntu服务端停止并用同一根目录重启后，Windows CLI查询 `MISSING_BLOCKS=503`，续传本轮发送503块并返回 `UPLOAD OK`。Windows与Ubuntu的SHA-256均为 `8db478eacc5c97f0d242e2145fe19af38a33a541ead56179380b93a20e80e3ff`。只读核对SQLite对应会话为 `COMMITTED`、`total_size=33554432`、`chunk_size=65536`、确认块数512。终端仅显示8行ACK而服务端保存9块，符合中断时最后一块已提交但客户端未打印回执的窗口。

## M3 现状审查（2026-11-01，本轮无实现修改）

- 核对当前 Windows 仓库与 Ubuntu `/home/ning/ChunkHarbor-m3-verify` 的96个源码、头文件、构建及测试文件：除 `apps/server/EchoHandler.cpp` 的尾部空格外一致；本轮开始时 Windows Git 工作区干净。
- Windows：按 `M3_VALIDATION.md` 的 Qt 6.8.3 / MinGW 13.1 参数重新执行 `cmake -S . -B build-windows-m3 ...`，随后 `cmake --build build-windows-m3 -j 2`、`ctest --test-dir build-windows-m3 --output-on-failure`，配置/构建成功，**9/9通过**。
- Ubuntu（SSH）：在上述验证目录执行 `cmake -S . -B build && cmake --build build -j 2 && ctest --test-dir build --output-on-failure`，配置/构建成功，Debug **30/30通过**，包含 `resume_integration`。
- Windows：执行 `python tests/integration/windows_cli_ssh_test.py build-windows-m3/chunkharbor-cli.exe miniob-ubuntu /home/ning/ChunkHarbor-m3-verify/build/chunkharbor-server`，真实隧道上传、会话查询、源文件变化拒绝及仅补缺失块均通过；仅使用脚本新建测试目录。
- 新发现（未修复）：`UploadStorage::commit` 在数据库已为 COMMITTED 后先删除 `meta.json`，再删除回执及目录；若清理中断，启动扫描却在检查 COMMITTED/EXPIRED 状态前强制读取 `meta.json`，导致整服务拒绝启动。启动清理自身也有相同的中断窗口。下一项应先补可重复清理与回归，再推进M4。
- 专项复现：通过 SSH 向 `python3 -` 传入一次性探针，复用 `resume_test.py` 帧辅助函数，在新建 `/tmp/chunkharbor-audit-cleanup-*` 目录正常上传提交；正常重启通过。随后人工构造清理中断状态（保留已提交数据库和正式文件，暂存会话目录仅有 `receipts.log`、缺少 `meta.json`），启动退出 **2**、stderr 为 `storage operation failed`；正式文件字节保持正确。补回原元数据后正常启动并清理目录。此为可达磁盘状态模拟，**并非在 unlink 指令窗口实测 SIGKILL 或硬件掉电**。
- 平台边界：Windows Qt CLI 已实现 `--state`、`status` 和续传；Linux CLI 的 `apps/cli/main.cpp` / `UploadClient.cpp` 仍仅提供首次上传，没有持久会话文件或续传命令。协议和服务端恢复测试通过不等于 Linux CLI 具备该功能。
- 本轮未重新执行 ASan/UBSan、Release、GUI原型测试、TSan、物理磁盘满、硬件掉电或性能基准；其余验收文档中的结果仍为历史记录。本轮仅更新进度文档，不修改实现、测试或README，不提交/推送。

## 发布检查（2026-10-31）

- 用户授权提交并推送 M2；README 的 Ubuntu 示例改为通用仓库路径和独立演示目录。
- 本轮 SSH `miniob-ubuntu` 连接成功，主机名 `ning-virtual-machine`；执行 `cmake --build build -j 2` 及 `ctest --test-dir build --output-on-failure`，Ubuntu Debug 构建成功，26/26 通过。

## 已完成

- [x] M3首个子任务：`src/storage/schema_v1.sql` 定义会话/确认块约束与单目标活跃会话唯一性；`docs/M3_SQLITE_DESIGN.md` 写明 ACK 事务边界及 COMMITTING 发布窗口恢复判据；独立 SQLite 测试验证重开持久性、外键、唯一约束。此项只验证 schema，不表示运行中的服务端已使用 SQLite。
- [x] M3第二子任务：`SqliteStore` 通过 RAII 打开/关闭 SQLite、要求 WAL/FULL/外键，空库执行 v1 事务迁移，再次打开验证版本，拒绝未知版本及非空无版本数据库。仅限磁盘线程调用；上传业务尚未调用。
- [x] M3事务接口基础：`SqliteStore::createTransfer`、`confirmChunk` 和 `confirmedChunks` 使用绑定参数；块记录及会话状态更新处于同一 SQLite 事务。独立测试覆盖重开查询、重复块拒绝及回滚。尚未接入 UploadStorage/网络业务，因此不是端到端续传。
- [x] M3查询协议起点：新增严格的 QUERY_SESSION 会话 ID 编解码及协议单测；服务端尚未处理 type 7，客户端尚未调用，不能视为查询功能可用。

M3事务基础验证（2026-10-31）：隔离目录 `/tmp/chunkharbor-m3-cpp` 重新构建后运行完整 CTest，**28/28通过**。首次测试使用早于创建时间的模拟时间戳，触发 `updated_at_ns >= created_at_ns` 约束而失败；改用当前纳秒时间后重跑通过。未执行服务端 kill -9/客户端重启续传验收，M3整体未完成。

M3第二子任务验证（2026-10-31）：虚拟机原工程有未提交改动，复制源码到 `/tmp/chunkharbor-m3-cpp` 隔离目录并放入本轮文件。虚拟机 SQLite 运行库已有，开发包缺失；用 `apt download libsqlite3-dev` 解压到 `/tmp/chunkharbor-sqlite-dev/extracted`，未修改系统安装。隔离目录执行 `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DSQLite3_INCLUDE_DIR=/tmp/chunkharbor-sqlite-dev/extracted/usr/include -DSQLite3_LIBRARY=/usr/lib/x86_64-linux-gnu/libsqlite3.so.0 -DOPENSSL_INCLUDE_DIR=/home/ning/ChunkHarbor/build/deps/openssl/usr/include -DOPENSSL_CRYPTO_LIBRARY=/usr/lib/x86_64-linux-gnu/libcrypto.so.3 -DCMAKE_CXX_FLAGS=-I/home/ning/ChunkHarbor/build/deps/openssl/usr/include/x86_64-linux-gnu`，随后 `cmake --build build -j 2` 与 `ctest --test-dir build --output-on-failure`，**28/28通过**。首次配置缺 OpenSSL 开发头，补现有私有缓存 include 后通过。未测试 Windows（该模块 Linux only）、ASan/Release、实际业务数据库事务及重启续传。

M3首个子任务验证（2026-10-31）：Windows `python tests/unit/schema_v1_test.py src/storage/schema_v1.sql` 通过；将这两个文件复制到虚拟机 `/tmp/chunkharbor-m3-schema-check/` 后执行同一 Python 测试通过。虚拟机正式目录执行 `cmake --build build -j 2` 与 `ctest --test-dir build --output-on-failure`，原有 M2 回归 26/26 通过；该目录未同步本轮 M3 文件，因此未运行新增 `schema_v1_test` 的 CMake 集成。未执行本轮完整重新配置/构建、真实 SQLite C++ 集成、重启续传或故障注入；这些不能计为通过。

- [x] M2：严格COMMIT_UPLOAD/COMMIT_RESULT及业务ERROR；真实CLI流式上传、同源FD身份检查、sent/confirmed区分；UploadHandler逐连接状态/请求关联、有界DiskExecutor业务投递及读取背压。
- [x] M2：UploadStorage实际块SHA-256与完整pwrite、fdatasync数据和同步追加回执后才ACK、整文件SHA-256、NOREPLACE发布及目录同步；同块/提交幂等、目标冲突与配额、断连保留。最低持久回执不等于SQLite/M3恢复。
- [x] M2：26项Ubuntu回归包含实际上传和恶意CLI响应、FIFO启动拒绝、容量1的粘包异步续作；独立审查发现问题后补红/绿测试并复核关闭。
- [x] M2可选：Qt6.8.3 MainWindow/QtTcpTransport连接状态原型，异步5秒握手、有界接收和错误状态；Windows实际构建/CTest 8/8通过，含7种Qt网络场景，截图已检查。没有GUI上传能力。
- [x] M2：新增Linux专用chunkharbor_server_core和TempFileStore，根FD固定、openat2受限解析、0600/CLOEXEC匿名O_TMPFILE、UniqueFd生命周期；路径/符号链接/目录替换竞态、故障注入和资源回归，见 `docs/TEMP_FILE_STORE.md`。M2实际缺口见 `docs/M2_REMAINING.md`。
- [x] M2：CHUNK_ACK五字段严格JSON、固定durable:true、ID/摘要hex互转、uint64 offset及长度/溢出检查、待确认DATA_CHUNK匹配；已接实际持久回执发送，见 `docs/CHUNK_ACK.md`。
- [x] M2：UPLOAD_PLAN三字段严格JSON、16字节ID与32lowerhex互转、块大小和uint64半开区间验证、512区间限制；不枚举块，安全计算文件块数并核对请求块大小。已接业务会话，见 `docs/UPLOAD_PLAN.md`。
- [x] M2：握手共享解析入口显式拒绝原始NUL，阻止合法JSON前缀隐藏尾随数据；HELLO/ACK/ERROR各类尾缀、前置/中间NUL、转义NUL、正常空白和4096字节边界单测，以及服务端/CLI集成回归。详见 `docs/HANDSHAKE.md`。
- [x] M2：BEGIN_UPLOAD严格五字段JSON；uint64文件大小/int64纳秒时间精度；UTF-8相对逻辑路径词法限制；块大小/小写摘要格式；拒绝重复/未知/缺失/错误类型/嵌套/超限/原始NUL尾缀。契约、设计取舍和命令见 `docs/BEGIN_UPLOAD.md`，不含文件或会话创建。
- [x] M2：DATA_CHUNK 60字节业务头显式大端编解码；完整长度、总payload上限、空数据、offset溢出检查；独立块对齐/整块/末块/文件范围检查。拥有解码数据；不引入OpenSSL/Qt/POSIX依赖。golden vector及FrameCodec组合测试，契约见 `docs/DATA_CHUNK.md`。
- [x] M2：独立可移植 chunkharbor_hash target，EVP 上下文 RAII、32字节摘要/64字符hex、内存块/增量/64KiB流式文件哈希；已知向量、二进制、读取边界和失败路径测试。约定与精确构建命令见 `docs/SHA256.md`。
- [x] M2：有界DiskExecutor、std::any结果与exception_ptr异常、独立eventfd完成通道、stop排空/析构取消及并发提交回归；约定见 `docs/DISK_EXECUTOR.md`。
- [x] M2首个子任务：严格HELLO/ACK/握手ERROR、版本/能力协商、客户端关联校验、逐连接握手状态、独立CLI hello与server --handshake模式。契约见 `docs/HANDSHAKE.md`。
- [x] M0：CMake、基础 RAII/日志、Linux server/CLI 入口。
- [x] 现有 Buffer：受限容量、整理/扩容、二进制流、5000次参考模型操作。
- [x] 单线程 LT EventLoop、EpollPoller、Channel，eventfd 跨线程唤醒；EINTR 重试、quit-before-run、有界 pending 队列（默认4096项）。
- [x] Channel weak owner 绑定和整个派发期间的强引用，回调移除后停止后续派发；生命周期回归测试。
- [x] TcpConnection：非阻塞读写、部分发送、EAGAIN/EINTR、按需 EPOLLOUT、有界输入/输出、读暂停恢复、半关闭排空。
- [x] TcpServer：loopback accept、连接上限、timerfd 空闲回收、资源不足暂停 accept、stop立即释放监听端口。
- [x] 纯 C++17 FrameCodec：32字节大端头、逐帧增量解析、长度/版本/类型/标志检查；Ubuntu与Windows相同golden vector。
- [x] 分离 protocol/client_core/linux_net targets，平台选项，Windows不编译Linux源码。
- [x] 可运行 Echo 服务端和非阻塞 Linux CLI；signalfd 受控停止。
- [x] 网络集成：100个同时连接、半包/粘包、1MiB帧、非法帧、EOF/RST、快速连断、慢读、FD恢复、空闲/数量上限、CLI超时。
- [x] 独立代码审查；修复停止后仍占端口的建议并自动化CLI沉默/截断响应超时场景。

## 本轮真实验证

M2完整上传本轮：Ubuntu正式目录Debug/ASan+UBSan/Release配置、构建与CTest均 **26/26通过**；Windows MinGW13.2纯协议 **7/7通过**、含hash **8/8通过**。精确使用/构建/测试命令见M2_USAGE.md，验收矩阵见M2_VALIDATION.md；Linux OpenSSL沿用已有私有开发头缓存。新增协议测试最初观察到的是缺header的编译失败，未冒充behavior-red；网络旧程序upload选项失败和应用暂停行为失败已实测。FIFO构造超时、容量1回调续作BUSY两个审查问题均先复现红测，再修复通过。receipt短写/EINTR/ENOSPC、数据及回执同步、发布后目录同步错误均有注入测试；不等同真实磁盘满/掉电。未运行TSan、真实断电/kill -9恢复、吞吐基准；SQLite/M3及完整GUI未实现。未提交/推送。以下为历史结果。

可选Qt原型：本机Qt6.8.3匹配MinGW Kit、Ninja、GUI=ON、server/CLI/hash=OFF，配置/构建/CTest **8/8通过**；真实截图无裁切。Qt独立只读审查未发现阻断问题。配置命令及启动SSH转发方法见M2_USAGE.md，未宣称GUI可上传或可分发打包。

Windows Qt→SSH本地转发→Ubuntu正式build服务端真实握手也通过：GUI --probe-port23587 经本地隧道至VM随机35087端口的--handshake服务，退出0；独占临时helper/服务进程经归属核对后已停止。此证据仅为连接握手，不是GUI文件上传。

TempFileStore前轮：Ubuntu正式目录Debug CMake配置/构建/CTest **20/20通过**；ASan/UBSan配置/构建/CTest **20/20通过**。Windows纯协议配置/构建/CTest **6/6通过**，未编译Linux组件；该轮未再运行Windows含hash配置。新增暂存单测先确认空实现失败，再接入实现通过。VM内核6.8，/tmp ext文件系统，支持openat2及O_TMPFILE；unsupported/EMFILE/EAGAIN/fchmod/fstat失败和EINTR通过链接包装注入覆盖。命令见TEMP_FILE_STORE.md。该轮未执行Release/TSan/GUI、真实bind mount/磁盘满、断电/恢复、发布或性能基准，未提交/推送。

本轮独立静态审查未发现阻断问题；根目录UID不匹配分支未做专门fstat注入，权限拒绝已测试。代码、测试及文档已同步正式虚拟机目录。

CHUNK_ACK前轮：新增单测先在空实现上canonical ACK断言失败，再接入实现。Ubuntu正式目录Debug CMake配置/构建/CTest **19/19通过**；ASan/UBSan配置/构建/CTest **19/19通过**。Windows MinGW13.2纯协议配置/构建/CTest **6/6通过**、含hash配置/构建/CTest **7/7通过**。精确命令见CHUNK_ACK.md（OpenSSL沿用既有配置）。该轮未运行Release/TSan/GUI、性能基准、GitHub Actions、落盘/断电/kill -9恢复测试；这些未实现，不能用该轮协议单测代替持久性证据。未提交/推送。

本轮独立审查未发现阻断问题，并用Windows既有构建执行ACK专项1/1通过；代码、测试与文档已同步正式虚拟机目录。

UPLOAD_PLAN前轮：Ubuntu正式目录Debug CMake配置/构建/CTest **18/18通过**；ASan/UBSan配置/构建/CTest **18/18通过**。命令见UPLOAD_PLAN.md，OpenSSL沿用已有缓存。Windows MinGW13.2纯协议配置/构建/CTest **5/5通过**，含hash配置/构建/CTest **6/6通过**。测试初稿因initializer_list混合const char*和string编译失败，修正后空实现按预期在golden断言失败，接入实现后通过；最终无测试失败。独立只读静态审查未发现阻断问题。该轮未运行Release/TSan/GUI、性能基准、GitHub Actions；未提交/推送。

握手NUL修复前轮：Windows先执行新增单测，确认3类消息共9个非法尾缀被接受；Ubuntu修复前 `ctest --test-dir build -R handshake --output-on-failure` **0/2通过**（单测及服务端集成均失败）。在共享parse入口增加显式NUL扫描后，Ubuntu Debug CMake配置/构建/CTest **17/17通过**，ASan/UBSan配置/构建/CTest **17/17通过**；Windows MinGW13.2纯协议 **4/4通过**、启用hash **5/5通过**。配置和全量命令沿用BEGIN_UPLOAD.md，专项命令为 `ctest --test-dir build -R handshake -V`。该轮未运行Release/TSan/GUI/性能基准/GitHub Actions；未提交/推送。

本轮独立只读审查通过，未发现新增阻断问题；代码、测试和文档已同步正式虚拟机目录。

BEGIN_UPLOAD前轮：Ubuntu正式目录Debug配置/构建/CTest **17/17通过**；ASan/UBSan配置/构建/CTest **17/17通过**。命令见BEGIN_UPLOAD.md，OpenSSL沿用前轮缓存；Windows MinGW13.2不启用hash配置 **4/4通过**，启用hash配置 **5/5通过**。先确认空实现的golden断言失败，再接入实现。独立审查发现原始NUL可绕过JSON库尾部检查，新增3类回归先复现失败，再在BEGIN_UPLOAD解析前拒绝原始NUL并重跑完整测试。该轮Release/TSan/GUI/性能基准、GitHub Actions未执行；未提交/推送。

前轮审查另用Windows临时探针确认原有 `decodeHello(encodeHello(Hello{}) + NUL + garbage)` 会接受；该历史问题现已通过本轮共享入口修复和单元/集成回归关闭。

DATA_CHUNK 前轮：Ubuntu正式目录 Debug CMake配置/构建及CTest **16/16通过**；ASan/UBSan配置/构建及CTest **16/16通过**。命令为 `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`、`cmake --build build -j 2`、`ctest --test-dir build --output-on-failure`；ASan同样操作build-asan并启用 `-DCHUNKHARBOR_SANITIZERS=ON`、设置检测环境变量。OpenSSL沿用前轮缓存的本地开发包配置，精确步骤见SHA256.md。Windows MinGW13.2：不启用OpenSSL的protocol配置构建/CTest **3/3通过**，启用hash配置构建/CTest **4/4通过**；命令见DATA_CHUNK.md/SHA256.md。新增测试先在空实现上golden vector断言失败，再接入实现通过。该轮Release/TSan/GUI/性能基准、GitHub Actions未执行；未提交/推送。

本轮 DATA_CHUNK 独立只读静态审查未发现阻断或重要问题；代码与文档已同步正式虚拟机目录。

SHA-256 前轮（2026-10-31）：Ubuntu 正式目录 CMake Debug 配置、构建及 CTest **15/15通过**；ASan/UBSan 配置、构建及 CTest **15/15通过**；Windows MinGW13.2 / OpenSSL1.1.1k Debug 构建及 CTest **3/3通过**。Linux 使用系统 OpenSSL3.0.2 运行库与无 root 解压的匹配开发包，未更改系统安装；配置路径及精确命令见 `docs/SHA256.md`。测试先以空实现执行并确认摘要断言失败，再接入 EVP 后通过。Windows 默认协议配置仍不要求 OpenSSL，显式 BUILD_HASH=ON 才构建哈希。该轮未运行 Release/TSan/GUI、EVP 提供者失败/OOM注入、性能基准；未接上传。

独立静态审查未发现阻断问题；补充读取有效前缀后失败、eofbit异常掩码和中文文件名测试。Linux CI 增加 libssl-dev 安装步骤，但本轮未推送，GitHub Actions 未执行。

DiskExecutor前轮：Ubuntu正式目录CMake配置、构建、CTest **14/14通过**；ASan/UBSan配置、构建、CTest **14/14通过**。新增disk_executor_test覆盖有界拒绝、多生产者、结果异常、线程归属、停止/取消、回调异常和普通loop队列满。Windows、Release、TSan该轮未执行（执行器为Linux组件）；下方握手/M1结果是此前历史。

本轮HELLO在Ubuntu执行CMake配置、构建、CTest：**13/13通过**；ASan/UBSan配置、构建及CTest：**13/13通过**；Windows MinGW13.2共享库构建及CTest：**2/2通过**。新增handshake_test、handshake_integration；原M1全部测试回归。精确构建命令沿用README，新增运行命令见HANDSHAKE.md。本轮Release/TSan/GUI未执行，以下11项结果为此前M1验收历史。

平台：Ubuntu 22.04，GCC 11.4，CMake 3.22.1，Python 3.10.12；正式目录 `/home/ning/ChunkHarbor`。

- Debug：`cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug`；`cmake --build build -j 2`；`ctest --test-dir build --output-on-failure`：**11/11通过**。
- ASan/UBSan：`cmake -S . -B build-asan -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCHUNKHARBOR_SANITIZERS=ON`；构建后设置 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1` 运行CTest：**11/11通过**。
- Release：`cmake -S . -B build-release -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release`，构建与CTest：**11/11通过**。
- Windows MinGW 13.2 / Ninja：禁用 server/CLI/GUI，构建 protocol/client_core 与协议测试：**1/1通过**。精确命令与验收对应见 `docs/M1_VALIDATION.md`。
- 原有 Buffer 的5项测试及之前事件循环的4项测试，是早期历史结果；当前以11项为准。

## 未验证与边界

- 用户人工“Windows CLI 中断→重启 Ubuntu 服务端→Windows CLI 继续上传”验收已通过。自动化另覆盖服务端 SIGKILL 后查询续传、Windows CLI 中断后状态文件续传与真实 SSH 隧道上传。
- 未做真实性能基准、TSan、硬件掉电和物理磁盘满测试；故障注入不等于真实硬件恢复。固定块增量已完成；Windows GUI上传、下载恢复、TLS/认证未实现。
- 单线程 LT Reactor 保留，SQLite 事务只由有界磁盘工作池执行；启动核验在监听前完成。每连接资源上限及单文件64MiB/65536块限制沿用M2；服务端仅监听127.0.0.1。
- 数据库和文件系统不是一个原子事务；`COMMITTING` 启动核验后才协调状态。异常数据隔离在 `.quarantine`，需运营者检查；30天未更新的 CREATED/RECEIVING 会话启动时自动清理其暂存文件。无历史快照和正式文件总容量配额。
- 旧M2无数据库记录的未完成会话无法自动升级续传，其目录在M3首次启动时隔离而非删除。根目录须私有且受信任，不对抗恶意本地 root/同UID 进程。

## 下一步（仅一项）

- [ ] M5首个最小任务：将已验证的任务/预览核心接入Qt TransferController和QAbstractTableModel，先做单文件备份状态展示与线程边界；按新阶段单独设计验收。
