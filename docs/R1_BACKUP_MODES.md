# R1：备份模式校验与扫描建议（2026-11-03）

> 本文记录前一轮严格手动模式校验。后续用户已授权默认auto与文件级计划，最新实现及验收见 [R1_AUTO_BACKUP](R1_AUTO_BACKUP.md) 和 PROGRESS 顶部；下文“不新增自动调度器”等是前轮范围记录，手动模式的严格语义继续保留。

本轮仅处理 GUI 备份模式、预览与执行的一致性。未修改健康检查、ComboBox、服务端、协议、schema 或共享 client_core；不新增覆盖确认流程、自动文件级调度器或并行上传。没有提交或推送。

## 原行为与确认的问题

- 三种 UI 模式均复用 `runDelta`。full 的安全条件是目标不存在（Absent），incremental 根据固定索引块哈希复用旧版本；resume 应查询本机保存的服务端会话。
- 原 `BackupConfig::resume` 实际是“自动复用检查点”开关，却同时控制显式 resume：关闭开关或没有检查点时，显式 resume 会退化为新上传。新增 worker 红测确认旧实现在无会话情况下仍成功预览。
- 原 full 预览直接使用 Absent，目标存在时只能报冲突，无法取得真实差异并给出合适建议。原 Unchanged 上传仍可执行 COMMIT，形成内容相同的新版本。
- 页面原来只禁用开始按钮，没有清除控制器保存的预览资格；retry 也可以绕开配置核验。部分文件成功后，旧 pins 的 Absent/Exact 基线已经过期。
- 真实服务端红测：A 成功、B 取消后点击 retry，A 因旧 Absent 报冲突，B 却继续上传提交，整批仍失败。修复后 retry 只重新扫描，显示 A 无需上传、B 可续传，再次确认前 B 不发布。

## 规则与最小设计

扫描始终用现有服务端 Preview/Auto 得到真实 `base_version`、差异计划；检查本机 Upload 检查点的根目录、本地路径、逻辑路径、端点、分块大小、大小和 SHA-256。匹配的未完成记录还须实际 QUERY_DELTA，核验 session ID、Upload 类型、基线、可续传阶段。服务端 Transferring/Verifying 位图决定剩余块，不把原始计划大小当成剩余量。

| 真实证据 | 建议 | 模式保护 |
| --- | --- | --- |
| 源身份匹配且服务器确认未完成会话 | 断点续传 | 显式 resume 不受自动开关关闭影响；full 仅可自动恢复原 full 会话，incremental 也须启用自动续传 |
| 没有待恢复会话，服务端目标不存在 | 首次完整上传 | full/incremental 可执行；显式 resume 拒绝，不新建上传 |
| 存在当前版本且内容有变化 | 增量备份 | full 拒绝已有目标，提示增量或新远端路径；resume 没有有效会话则拒绝 |
| 存在当前版本，服务端计划 Unchanged 且无缺失块 | 无需上传 | incremental 可核验后跳过；full 仅对本任务匹配的已完成 full 记录允许跳过，其他已有目标仍保护 |
| 检查点损坏、源不匹配、会话失效或基线变化 | 待核验/错误说明 | 保留原检查点，阻止执行；恢复原源文件/配置、核验服务器，或使用新远端路径，不清空状态后偷偷重传 |

显式 resume 遇到已 COMMITTED 会话时，调用原 `runDelta` 核验结果中的路径、大小、摘要和 committed 标志。若提交回包丢失，恢复真实历史回执；再次扫描确认当前版本未变化才允许无操作完成，不把它当成未完成会话推荐。目录含未开始文件时，严格 resume 会阻止整个预览开始；用户可选择 incremental 并重新确认，保留既有自动续传策略。不会悄悄切换模式。

`BackupAdvice`、`modeAllowed`、`resumeSession` 是内存中的证据分类，未写入旧历史/检查点格式。TaskModel 汇总这些字段，而非根据“新增/修改/未变”计数猜模式。混合目录展示首次、增量、续传、无需上传、待核验各类数量及不适配数量；建议放在原变更预览说明区，不增加卡片或调整其他页面布局。

执行仍是同一后台线程顺序处理文件，每个 Peer 的 QTcpSocket 在该线程创建/使用/销毁。GUI 控制器通过 queued 调用更新模型；新增建议没有跨线程访问控件。预览的 SHA-256/大小/路径与配置绑定；模式、目录、前缀、连接设置变化会清除控制器 pins。所有正式执行结束（成功、失败、取消）都使旧预览失效；“重试 / 恢复”按原配置重新扫描，用户确认后再开始。

正式执行使用预览的 Absent/Exact 基线并复核源快照。无需上传也进行 Exact 的 Preview 协商，遇到外部版本变化拒绝旧计划，既不发 DATA_CHUNK/COMMIT，也不制造成功 versionId。服务端提交时现有的基线检查与整文件校验保留；不做跨文件原子承诺，部分文件失败仍逐文件记录。

## 修改文件

- `apps/gui/BackupWorker.cpp`：实际会话核验、模式保护、剩余位图、无需上传及提交回执恢复。
- `TransferTypes.h`、`TaskModel.cpp`：独立于显示文案的分类字段和统计建议。
- `TransferController.h/.cpp`：建议通知、预览失效、重试重新扫描。
- `pages/BackupPage.cpp`、`pages/SettingsPage.cpp`：预览说明和自动续传开关语义。
- `tests/unit/gui_worker_test.cpp`：无会话显式 resume 红测与会话断言；`gui_transfer_probe.cpp`：输出真实分类、模式切换/重试路径及可见文案检查；CMake 为探针加入原 styles.qrc，便于原生截图。
- `tests/integration/windows_mode_ssh_test.py`：新增隔离矩阵；`windows_gui_ssh_test.py`：更新旧混合目录的静默续传预期，先断言严格拒绝，再显式选择 incremental 扫描执行。

## 实际验证与复现命令

Windows 11 / Qt 6.8.3 / MinGW 13.1 / Ninja / Debug，复用 `build-windows-gui`；Ubuntu 使用现存 `/home/ning/ChunkHarbor/build/chunkharbor-server`。本轮未同步或重编译 Ubuntu 源码，端到端验证的是新 Windows 客户端与现存协议兼容。测试创建临时源目录、独立状态目录、随机端口和专用服务端根；不读取真实备份。新脚本停止自身 PID/隧道，保留 `/tmp/chunkharbor-mode-*` 夹具供核对。

```powershell
$env:PATH='D:/QT/Tools/mingw1310_64/bin;D:/QT/6.8.3/mingw_64/bin;'+$env:PATH
cmake -S . -B build-windows-gui
cmake --build build-windows-gui -j 2
ctest --test-dir build-windows-gui --output-on-failure
python -X utf8 tests/integration/windows_mode_ssh_test.py build-windows-gui/chunkharbor-gui-transfer-probe.exe miniob-ubuntu /home/ning/ChunkHarbor/build/chunkharbor-server
python -X utf8 tests/integration/windows_gui_ssh_test.py build-windows-gui/chunkharbor-gui-transfer-probe.exe miniob-ubuntu /home/ning/ChunkHarbor/build/chunkharbor-server
python -X utf8 tests/integration/windows_gui_rescan_test.py build-windows-gui/chunkharbor-gui-transfer-probe.exe miniob-ubuntu /home/ning/ChunkHarbor/build/chunkharbor-server
```

配置/完整构建退出0；最终串行完整 CTest **24/24通过，127.99秒**（不是性能指标），本轮worker 25.03秒、原生ComboBox 28.53秒。中途完整CTest曾23/24，仅ComboBox原生鼠标展开失败；单项重跑1/1通过28.94秒，最终完整也通过，仍保留间歇失败记录。原生桌面测试与模式截图测试应分开运行，失败根因尚未定位，本轮不修改ComboBox。

三个 SSH 脚本最终均退出0：新模式矩阵根`/tmp/chunkharbor-mode-p540xG4b`，既有GUI上传/崩溃恢复根`/tmp/chunkharbor-m5-data-JnIixQ6z`，既有重扫回归根`/home/ning/ChunkHarbor/build/rescan-data-xkAQMHkX`（最后一项由原脚本带路径校验清理）。服务器和隧道均已结束；未同步源码、未重跑Linux全量。

新矩阵检查全新/已有变化/未变化、错误 full/resume、自动开关关闭时显式续传、源大小及同大小内容变化、损坏 JSON、失效服务器会话、模式切换、部分成功后的重试、混合目录、预览后外部提交竞争；利用 SQLite **只读查询**确认无需上传不增加版本、重试未确认不发布文件，并逐文件比对服务端 SHA-256。旧GUI脚本仍覆盖中文/空文件/排除、单块变化、GUI进程kill+服务端SIGKILL后恢复、暂停取消和关窗；旧重扫脚本仍覆盖A/B切换及缺失/锁定源等边界。

真实 Windows MainWindow 探针在当前DPR1.25截图`build-windows-gui/mode-advice.png`已查看，显示四类混合状态及可见分类建议；QSS来自正式样式资源。截图不受版本控制，自动测试也检查建议在可见QLabel出现、模式保持用户原选项。

## 失败记录与剩余边界

- 初版 QUERY Peer 在同一作用域内保留到第二个恢复连接创建之后，单连接假服务端夹具超时；改为局部立即调用的 lambda 返回响应，先析构 socket 再建下一连接，worker 回归通过。这是 RAII 作用域与连接资源寿命的实际区别。
- 新 SSH 测试初版把状态目录和外部源目录命名相同，扫描到测试日志/状态并报源变化；修为独立 `states/` 后重测，未为错误夹具放松源校验。
- 原集成测试把混合目录的无会话新文件也当成 resume 成功，按本轮严格语义更新断言并保留哈希验证。
- 原生 ComboBox 桌面自动输入存在间歇失败：全量曾在 `mouse opens actual popup` 断言失败，单项复跑通过。未改该组件，也未据复跑抹去失败记录；详见 PROGRESS。
- 单服务器可信 loopback/SSH、文件上限 64MiB、串行上传、原请求超时不变。协议 QUERY_DELTA 不提供完整源身份列表，必须同时有本机匹配检查点；丢失本机状态时不自动发现/接管远端在途会话。
- 端点仍以 `127.0.0.1:端口` 关联；同一端口转到另一台服务器时会话核验失败，不自动丢弃状态或迁移。未加入 server UUID、全局会话检索或新的协议字段。
- full 遇已有目标选择拒绝并给安全建议，未增加覆盖确认对话框。recommendation 是扫描时点的证据；执行重新检查，不能宣称扫描会锁住整个目录或远端。
- 未验证 Linux Qt GUI、MSVC、Release/Sanitizer、恶意同权限修改检查点、真实硬件故障或长时间压力。未开展本轮多档 DPI/人工 GUI 操作验收；截图来自真实 MainWindow 测试探针，不等于完整手工验收。

本项完成后停止；R1 全阶段尚未据此宣称验收。
