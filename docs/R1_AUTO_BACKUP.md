# R1 自动备份与文件级计划（2026-11-03）

> 阶段收尾补充：用户人工确认下拉框无实际故障并明确要求结束R1；业务实现与既有测试证据结合本次用户验收，R1现已完成。下文最新自动化全量23/24及未验证边界按原样保留，不宣称全部自动化通过。阶段完成记录见 [PROGRESS](PROGRESS.md)，R2未自动启动。

## 本轮设计与执行计划

用户已明确本轮设计和执行授权：新增默认 auto，保留严格手动 full/resume/incremental；复用现有差异协议，仍串行执行，不实现并行、FastCDC、恢复或鉴权，不提交/推送。

采用现有 `BackupConfig` 作为目录级配置、`QList<FileRecord>` 作为内存文件计划。新增 `BackupStrategy { Error, Full, Incremental, Resume, Skip }` 和完整扫描标志 `RunResult::planComplete`；不新增计划文件格式。Advice表示实时证据分类，strategy表示经过当前模式校验的实际动作。

规则：匹配且可恢复会话优先Resume；无会话且Absent选Full；已有版本变化选Incremental；当前内容一致选Skip。auto不受原手动自动续传布尔开关禁用影响；手动模式保持原含义。旧检查点继续只记录full/incremental原始上传语义，恢复已有会话保持其原模式。无效会话/损坏状态保留并标Error，本轮选择保守拒绝，不自动清空后重传。

完整扫描后auto可以确认执行安全行；Error行保留错误、不再尝试上传，总结果是部分成功。全局扫描/状态错误、取消、目录集合变化仍阻止执行。执行重新验证源、会话和固定基线，自动计划的策略/会话不得悄悄变化；任何终态使预览失效，retry先重扫再确认。

新用户默认auto；已保存的手动mode仍保留，未知mode回退auto且不会继承旧预览。新增执行策略列及分类数量/错误/预计字节，不改变其他页面布局。后台线程及Qt queued模型通知不变。

实施步骤（使用TDD；仅本轮任务）：

- [x] 扩展真实SSH auto矩阵及默认模式/旧配置测试，运行旧代码观察缺失auto失败。
- [x] 修改TransferTypes、BackupWorker、TransferController：文件计划、严格复核、完整扫描后的局部失败隔离；相关worker回归。
- [x] 修改TaskModel、BackupPage、SettingsPage及探针：默认auto/手动兼容、执行策略、分类统计和可见说明。
- [x] Windows配置/构建/CTest，auto矩阵及原手动、上传、重扫SSH回归；记录失败及未验证项（CTest最终23/24，原生输入专项仍失败）。
- [x] 更新PROGRESS、模式说明和Windows使用文档；保留本轮前所有改动与历史，不继续R2。

需特别验证：损坏检查点影响范围；续传在扫描后变COMMITTED/失效/换会话不得变成其他动作；无变化不增版本；源或远端竞争导致部分失败后的重扫；旧手动默认/会话与配置兼容。

## 实际结果

### 修改前后

原来扫描得到服务端证据后，还要让每个文件通过同一个手动模式；任一不适配就整批不可执行。现在auto先生成混合文件计划，确认后逐行执行；手动模式仍有原来的整批严格校验。复用同一串行worker和每文件独立Peer，没有引入调度线程池、协议命令或新依赖。

| 文件证据 | 计划动作 | 执行前保护 |
| --- | --- | --- |
| 本机身份匹配且QUERY确认未完成Upload会话 | Resume | session ID/类型/阶段/base一致；额外用固定基线Preview核验当前远端，再查询缺失位图；不使用内存进度 |
| 无待恢复会话，当前版本Absent | Full | 源摘要/大小/路径匹配，Absent仍成立；新checkpoint保留旧full格式 |
| 当前版本存在且差异计划不是Unchanged | Incremental | 源匹配、Exact版本仍成立，复用固定索引块；新checkpoint保留旧incremental格式 |
| 当前版本存在且Unchanged、缺失0块 | Skip | 再次进行Exact的Preview核验，不发送DATA_CHUNK/COMMIT，不写虚假versionId |
| 会话/检查点/源不匹配或不可判定 | Error | 保留状态、错误不获上传权限；修复后只能通过重新扫描生成新计划 |

正式执行固定预览中的策略与会话；如果Resume在扫描后已提交/失效、换会话，或源/基线变化，不会在原授权下改为Full/Incremental。已提交回执丢失的恢复仍由核心校验真实committed结果；扫描恢复后动作是Skip，而不是误称未完成Resume。

自动模式执行Error pin前直接保留错误，不因文件后来恢复就自行上传。全目录扫描/状态预检失败及取消不能授权部分计划；存在扫描错误时使用`planComplete`与至少一行安全策略共同授权。对已扫描文件的损坏checkpoint移入每文件隔离校验；无法关联到当前文件的损坏状态仍保守阻止整批。未完成会话的源缺失/被排除会显示Error，保留其原记录。目录条目集合变化仍拒绝旧计划。

### 文件与接口

- `apps/gui/TransferTypes.h`：默认auto；BackupStrategy内存字段及RunResult::planComplete，旧持久结构不变。
- `apps/gui/BackupWorker.cpp`：接受auto、逐行计划、错误pin隔离、保留原checkpoint语义、续传前固定基线核验。
- `apps/gui/TransferController.cpp`：仅完整扫描可授权安全子集；全局失败明确“核验失败/扫描未完成”，不展示可确认的零错误计划。
- `apps/gui/TaskModel.cpp`：第六列执行策略、策略名tooltip、分类计数/预计字节；`pages/BackupPage.cpp`：auto选项及默认、部分预览提示、仅为策略列保留可读宽度；`pages/SettingsPage.cpp`：明确旧布尔开关只影响手动full/incremental。
- `tests/unit/gui_worker_test.cpp`：auto新文件、续传基线响应异常时0DATA、扫描上限不生成完整计划；`gui_state_test.cpp`：策略列及旧历史精确整数保留；`gui_pages_test.cpp`：新默认、三种旧偏好及全局失败；`backup_layout_test.cpp`：新列文字空间和原布局回归。
- `tests/integration/gui_transfer_probe.cpp`：输出strategy/ready、配置切换和部分计划确认路径；`windows_mode_ssh_test.py`在原严格手动矩阵后新增auto矩阵。不改CMake、共享库、服务端、健康检查、ComboBox或QSS。
- 文档更新PROGRESS、BUILD_WINDOWS、GUI_DESIGN及前轮R1_BACKUP_MODES入口；旧轮记录不改写。

### 构建与测试证据

Windows11 / Qt6.8.3 / MinGW13.1 / Ninja / Debug，复用既有`build-windows-gui`。Ubuntu仍使用原工程现有server，本轮不复制源码、不改后端，不运行Linux全量。

```powershell
$env:PATH='D:/QT/Tools/mingw1310_64/bin;D:/QT/6.8.3/mingw_64/bin;'+$env:PATH
cmake -S . -B build-windows-gui
cmake --build build-windows-gui -j 2
ctest --test-dir build-windows-gui --output-on-failure
python -X utf8 tests/integration/windows_mode_ssh_test.py build-windows-gui/chunkharbor-gui-transfer-probe.exe miniob-ubuntu /home/ning/ChunkHarbor/build/chunkharbor-server
python -X utf8 tests/integration/windows_gui_ssh_test.py build-windows-gui/chunkharbor-gui-transfer-probe.exe miniob-ubuntu /home/ning/ChunkHarbor/build/chunkharbor-server
python -X utf8 tests/integration/windows_gui_rescan_test.py build-windows-gui/chunkharbor-gui-transfer-probe.exe miniob-ubuntu /home/ning/ChunkHarbor/build/chunkharbor-server
```

实际配置/完整构建退出0，最终完整CTest **23/24通过，110.63秒**。`combo_popup_test`失败13.07秒，随后专项仍0/1失败2.39秒，原因是原生自动鼠标未展开菜单。其余23项通过；不能称GUI全量回归通过。该测试同轮在另一全量中曾通过28.90秒，但当时新增全局失败提示红测还未修复，整套当时同样不全绿。不修改无关弹层来消除失败。

最终探针模式矩阵（含手动和auto）退出0，根`/tmp/chunkharbor-mode-Rfj1b0j7`；最终重扫回归退出0，根`/home/ning/ChunkHarbor/build/rescan-data-722wYAdS`（原脚本清理）。既有GUI上传/进程崩溃恢复本轮worker业务实现后通过，根`/tmp/chunkharbor-m5-data-L804Bpbt`，此后仅策略列显示及全局失败文案修正。最终截图`build-windows-gui/auto-advice.png`已查看，实际DPR1.25下四种策略文字完整可读；未做本轮多档DPI/物理输入验收。测试仅专用源/状态/根目录、随机端口、自建服务及SSH隧道；均已结束，没有操作真实备份。

完整记录见PROGRESS顶部，以下失败记录也保留。

- 旧代码红测：auto不被接受、缺少执行策略列，2/2失败；初稿测试回调引用尚未定义的变量导致编译失败，修正夹具后才得到真实行为红测。实现后worker/state/pages初步3/3通过。
- 额外红测：存在已排除行时，全局预检失败仍可能显示“0错误可确认计划”；按钮本来就禁用。用真实后台控制器夹具复现后改为依据planComplete显示失败，未放松任何上传权限。
- 真实矩阵覆盖新文件、已变化、Skip不增版本、旧full会话在auto及关闭旧开关时恢复、丢失提交回执恢复为Skip、全混合、损坏JSON/会话失效与状态字节保留、仅错误不可执行/有安全行可部分执行、源变化、模式/前缀/目录修改、部分成功重扫重试、扫描后会话变化以及远端新版本竞争。
- 竞态夹具初稿想让第二个任务向正在续传的同一目标提交，实际被原服务端target-busy保护拒绝。这是保护生效，不能声称已复现版本竞态；调整为断言此保护，并单独测试扫描后会话失效、无在途任务的实际版本更新。续传基线错误在本地TCP测试中验证0DATA，明确区分真实server和可控peer证据。
- 中途全量22/24：ComboBox原生鼠标展开失败，文件列表cancel夹具75ms取消可能早于假服务端收到完整请求，出现unexpected client disconnect；未改这些无关功能。后续单独按顺序重跑，结果见最终记录。一次重扫启动与探针链接重叠报Windows文件占用，纠正验证顺序后再测；不将其记为上传缺陷。

### 兼容性、知识点与边界

`mode`是用户目录策略，`resume`布尔值只决定手动full/incremental是否自动复用匹配会话；auto和手动resume必须核验有效会话。已有偏好中的手动mode保留，未知mode选择auto但不会继承旧预览。新增strategy与planComplete仅在内存，WorkspaceStore历史、检查点、协议和SQLite格式不变。auto新上传记录明确存full/incremental，保持旧客户端可解析的格式；本轮测试当前手动resume读取auto产生的状态及旧full会话被auto恢复，没有运行旧GUI二进制。

重要C++/Qt设计：enum class区分“证据分类”和“执行权限”；配置与pins按值捕获给后台线程，避免运行中UI修改计划；queued通知保持模型在GUI线程；RAII使每次查询/额外Preview的socket先析构再进入下一连接；SHA-256与Exact/Absent构成乐观并发校验，预览不是锁；只有真实提交响应/COMMITTED校验结果进入成功历史，Skip与Error都没有新成功版本。

单文件64MiB、原超时、串行模型和已有协议限制均保留。额外续传Preview有清单/哈希成本，本轮不宣称性能提升。无效会话保守拒绝，不自动删除或创建安全替代备份；用户可恢复原配置或选择新远端路径。无法关联的损坏状态是全局错误；无法识别服务器身份、丢失本机检查点、转发端点变化仍需原安全规则处理。未验证Linux Qt/MSVC/Release/Sanitizer、真实磁盘硬件故障、长时间压力或物理人工输入/多屏DPI。截图来自实际MainWindow探针，不等于人工端到端验收。
