# 灵活备份范围与文件级自动策略

日期：2026-11-05。开发基线为 `194ae7f`。开发完成后，用户另行授权提交、推送 GitHub 并同步 Ubuntu；两端版本以 Git 提交号核对。本轮不改变服务端协议、SQLite schema、历史版本或快照语义。

## 从目录扫描到明确的候选集

以前只能选择目录，`DirectoryScanner` 递归枚举，`.git;build` 按目录名称精确排除。扫描根同时用于远端相对路径和检查点身份。选择更深的目录，会改变同一文件的远端路径。没有现成的显式文件列表扫描接口。

现在在备份页选择范围，指定本地映射根和远端相对路径，扫描预览，核对最终逻辑路径、过滤原因和每个文件的执行策略，最后确认备份。默认仍为整个文件夹、包含隐藏属性、auto；旧配置不需要迁移。

| 范围 | 枚举行为 |
| --- | --- |
| 整个文件夹 | 沿用递归扫描和全局排除 |
| 单个文件 | 只检查所选文件及其祖先属性，不枚举兄弟文件 |
| 多个指定文件 | 只处理明确列表；可重复添加不同子目录中的文件，也可清空重新选择 |
| 按规则筛选 | 递归枚举，先包含筛选，排除最终优先；明确排除的目录直接剪枝 |

规则输入只在“按规则筛选”时出现；设置页全局排除、状态目录保护和隐藏属性选项对全部范围生效。单文件和多文件的父目录不会作为新的递归扫描范围。

## 路径身份和检查点

```text
最终逻辑路径 = 远端相对前缀 + 相对于本地映射根的路径

映射根 D:/testFiles
扫描范围 D:/testFiles/成都博物馆
选中文件 D:/testFiles/成都博物馆/111
远端前缀 留空
最终路径 成都博物馆/111
```

固定映射根后，改为只选择 `111`，仍得到 `成都博物馆/111`。如果明确将映射根改为 `成都博物馆`，则得到 `111`，这是新的逻辑身份，既有版本不会迁移或按哈希合并。GUI 始终显示映射公式、根改变的影响，表格显示最终远端路径，悬停可看本地完整路径、预计上传量和原因。

目录模式的映射根留空时，保持原来的“相对于扫描目录”行为。文件模式第一次选文件时建议公共父目录，之后添加文件不会偷偷扩大映射根；跨子目录需要手动固定共同项目根。不同磁盘上的文件不能映射到同一个本地根，需分任务处理。根外路径拒绝；不存在或非普通文件标错；重复目标（Windows 上大小写不敏感）拒绝。远端路径仍经原有 `encodeBeginUpload` 验证，不能传 Linux 物理绝对路径。

检查点仍用有效映射根、逻辑路径、端点构造原有键，并核对本地路径、大小、SHA-256、分块参数和服务器会话。目录模式留空根时，旧键和身份完全保留。显式填写相同根仍保留旧整目录任务的缺失源检查。显式文件候选逐个验证身份；真正的子树、规则或隐藏筛选任务不把选外检查点视为缺失源，也不删除它们。文件在预览后消失，执行重枚举及固定计划复核会拒绝过期计划。部分范围在新一轮预览前已经消失且没有明确选中的文件，不被推断为本次任务的成员，也不意味着远端删除。

## 规则语义

规则以分号分隔，每组最多 128 条，每条最多 1024 个 UTF-16 单元。不兼容完整 `.gitignore` 语法。

| 写法 | 含义 |
| --- | --- |
| `README.md` | 任意层级的精确文件名；任务排除也匹配同名目录并剪枝 |
| `*.cpp;*.h;*.hpp` | 按文件名后缀筛选；`*` 匹配零或多个非 `/` 字符，`?` 匹配单个字符 |
| `src/main.cpp` | 相对于映射根的精确路径 |
| `./README.md` | 映射根下的文件，不能匹配子目录内同名文件 |
| `build/` | 任意层级同名目录及其后代；包含规则也支持目录子树 |
| `vendor/cache/` | 映射根下的指定目录及其后代 |

空包含集表示候选文件全选，多条包含取并集，多条排除取并集，排除覆盖包含。包含后缀规则不会阻止遍历其父目录。排除目录时不读取后代；显式选择其后代或把扫描根放在其深层，也不会绕过祖先排除。全局和任务排除取并集。

旧全局纯名称 `.git;build` 继续仅匹配目录，保持原 UTF-8 名称集合的大小写敏感语义，不会排除名为 `build` 的普通文件，也不会新增排除 `Build` 目录；全局相对路径和通配符为新增显式语法。任务排除纯名称可匹配文件与目录。`/`、`\` 都规范为逻辑分隔符；拒绝绝对路径、`..`、空段、`**`、字符组 `[ ]`、否定规则和控制字符。无效规则在网络请求前报错，详情保留在日志中。

Windows 新增规则使用 Qt Unicode 大小写不敏感模式；旧全局纯目录名称是上述兼容例外，需要全局忽略目录名大小写时可显式改用 `build/`。不做 Unicode NFC/NFD 归一化。Linux 可选 GUI 的匹配区分大小写；本轮未构建 Linux Qt GUI。Windows “包含隐藏属性”默认勾选，取消后按 `FILE_ATTRIBUTE_HIDDEN` 过滤文件和剪枝目录。`.gitignore`、`.env` 等点文件并不自动成为隐藏文件。没有敏感内容分析；用户负责选择应备份的内容。Linux 不用点文件模拟 Windows 属性，隐藏属性开关不额外过滤 Linux 文件。

## 自动计划、资源和安全

`BackupSelection` 只决定候选文件和逻辑路径，不决定上传策略。每个参与文件仍进入原有 `runBackup`、`DeltaExchange`、full/incremental/resume/skip/error 和 Exact/Absent 基线验证；三种手动模式保留。不存在有效会话不会静默续传，旧版本不会因本次排除被删除，部分备份不会自动生成完整目录快照。

`sameConfiguration` 纳入 scope、显式文件列表、映射根、包含/排除规则、隐藏选项及原连接配置。所有对应控件变化都使预览失效，执行时再比较完整配置、重枚举同一范围、复核固定文件集合、源摘要和远端基线。文件模式不会在执行阶段扩大扫描。

扫描、属性检查、规则匹配、哈希、检查点和网络任务都在原有 worker 线程中执行。`QTcpSocket` 仍在创建它的任务线程中使用，GUI 通过 queued 投递更新模型，取消/关闭仍由 `RunControl` 和线程析构等待收敛。文件读取复用 Windows 祖先目录句柄和普通文件句柄的 RAII、安全属性复核及哈希检查。枚举时也拒绝源符号链接、重解析点和根外路径，不用字符串前缀代替边界判断。

最多 10,000 个预览条目（含剪枝/过滤/错误项）、文件上限 64 MiB，沿用目录深度限制；不增加并行上传。完整候选集有界缓存后才请求服务器。被过滤的文件不做内容哈希、会话查询或传输；仅可读取大小等元数据。已检查文件数、过滤文件数、参与文件数和剪枝目录数分别统计；没有遍历的子树不伪造文件数。过滤项不计为“内容未变化”“上传成功”或“失败”。未完成扫描不显示完整统计。

## 修改入口

- `apps/gui/BackupSelection.{h,cpp}`：候选集、公共根建议、规则编译、映射和过滤。
- `TransferTypes`：范围配置和范围统计；`BackupWorker`：候选集接入、身份兼容。
- `TransferController`：配置绑定、统计与旧预览失效；`TaskModel`：逻辑路径、原因和预计字节。
- `pages/BackupPage`：四种范围、文件/目录选择、映射和规则控件；`SettingsPage`：全局规则说明。
- 共享 `DirectoryScanner`：增加可选纯 C++ 过滤回调和过滤原因；旧 CLI 构造接口使用默认回调，语义保持。
- `CMakeLists.txt`、`backup_selection_test`、`gui_worker_test`、`backup_layout_test`、`gui_transfer_probe`、`windows_scope_ssh_test.py`：构建及验证。

## 实测记录

A 闭环：`backup_selection_test` 与 `gui_worker_test` **2/2**；选中列表映射、同名文件、65 MiB 未选兄弟文件不扫描，原 worker 会话回归。

B 闭环：先观察规则测试因未过滤 65 MiB 文件失败，再接入过滤回调。`backup_selection_test`、`directory_scanner_test`、`gui_worker_test` **3/3**；包含目录后代和审查发现的祖先排除问题也分别先复现失败再修复。

C 闭环：Windows GUI 全目标构建成功，完整 CTest **33/33**，含原生 ComboBox 自动交互和备份页布局；完整跑次耗时 249.18 秒，仅作为验证记录。收尾审查发现“切回 Folder 后隐藏的非空规则仍影响 missing 检查”问题，新增回归先观察失败，再删去不生效规则的条件；重新完整构建，受影响的 `backup_selection_test`、`backup_layout_test`、`gui_worker_test` **3/3**，并重跑真实范围 SSH 测试通过。源码审查还修复了深层扫描根绕过祖先排除、根大小写误报、显式相同根遗漏 missing 防护等问题。

最终兼容性核对将旧全局纯目录名继续保持为大小写敏感。为此新增 `other/Build/legacy.cpp` 用例；首轮夹具误用与 `build` 同一父目录的 `Build`，在 NTFS 上实际指向同一个目录，已修正为不同父目录。新任务规则仍按 Windows 大小写不敏感匹配。最终 GUI/相关目标构建和上述 **3/3** 已重跑通过，日志 `scope-compat-build.log`、`scope-case-build.log`、`scope-compat-ctest.log`；最终源码真实范围 SSH 也再次通过（`scope-ssh-final.log`）。

布局自动测试检查整个文件夹和规则范围，1280×820、1440×1100、1060×700、最大化和恢复窗口，空/3/200 行，包含表格滚动和高级控件显隐。原生 Windows 预览截图 `build-windows-gui/scope-preview.png` 已查看。以上属于自动运行窗口和几何检查，不是人工逐项文件对话框验收。此前完整跑次原生 ComboBox 焦点曾失败，且一次布局测试启动与链接过程重叠导致 BAD_COMMAND；构建结束后完整重跑 33/33，未修改或降低 ComboBox 测试。

既有 `windows_mode_ssh_test.py` **12 组 PASS**，覆盖手动模式、原检查点、显式续传、损坏/失效会话隔离、混合自动计划、源变化、远端竞争、重试必须重新确认。额外 SSH 回归：当前单文件下载（空文件/中文/多块）、真实 v2 夹具迁移后 A/B/C 指定历史恢复、旧/新快照恢复、当前文件夹下载均通过。日志保存在 `build-windows-gui/scope-*-regression.log`。

Windows 命令（Qt 6.8.3、MinGW 13.1、Ninja，Debug）：

```powershell
$env:PATH = 'D:/QT/Tools/mingw1310_64/bin;D:/QT/6.8.3/mingw_64/bin;D:/CMake/bin;' + $env:PATH
cmake -S . -B build-windows-gui
cmake --build build-windows-gui -j 3
ctest --test-dir build-windows-gui --output-on-failure
python tests/integration/windows_scope_ssh_test.py build-windows-gui/chunkharbor-gui-transfer-probe.exe miniob-ubuntu /tmp/chunkharbor-scope-93DFYtuz/build/chunkharbor-server
```

Ubuntu 使用 `/tmp/chunkharbor-scope-93DFYtuz` 的源码副本和 Debug 构建，GUI OFF、server/CLI/tests ON，复用现有 OpenSSL/SQLite 开发头和系统共享库；`cmake --build ... -j 2` 成功，`ctest --test-dir ... --output-on-failure` **57/57**。正式 `/home/ning/ChunkHarbor` 和真实备份库未部署或修改。

真实 Windows→SSH→Ubuntu 专用测试根 `/tmp/chunkharbor-scope-data-5moimPZJ`，最终源码复测根 `/tmp/chunkharbor-scope-data-s70Iee8v` 已验证：整个目录、单文件、多文件同名、中文、规则剪枝、排除后远端保留、16 MiB 单文件取消/续传、多文件同时规划 full/incremental/resume/skip、不同映射身份、选外缺失 checkpoint 不误报，以及 scope/root/rules/hidden/list 变更旧计划拒绝。以下为服务器 `sha256sum` 与本地 `hashlib.sha256` 一致的实际值：

| 文件/阶段 | SHA-256 |
| --- | --- |
| `scope/src/main.cpp` 首次 | `02c9a68f567b8ec215d893cde67ae2da41ba3c9b838d0ba4d3d04ab3cdc58da6` |
| 同文件修改后增量 | `3e28b66ee16c171ee62d7afb407d42d84ffa47c87de76549f3bbda814b453ad5` |
| `scope/include/main.cpp` | `7abaa701a6f4bb8d9ea3872a315597eb6f2ccfd03392d8d10560837f6136d06a` |
| `scope/data/resume.bin` 取消后续传 | `05a39e7abbd20c247595df52aae628db1edd38f3c3590acdfd9749f445e0d367` |
| `scope/README.md` 未变化 | `711a6108ba2ce6ca93dd47d6817f2361db10d8ab6eec89460b2dfc2c325efabe` |

旧 v2 夹具迁移恢复使用 `/tmp/chunkharbor-history-ssh-BYVXnfQi`：A 为 `420dbf371827b6075d7aa33200540df7bddc39ed00e5ee982486061678523c21`，B 为 `bd71325ea42140323bf1ce3b3f78b488a4b26f6c3edda9438f45ad96fc3bc468`，C 为 `7a2fb88ecfd47ffd4d45cd3aac517bf90539a7aaa5eee908028f30b6a581828f`，各 14 字节，均与真实不可变对象一致。快照恢复使用 `/tmp/chunkharbor-history-ssh-pluhGt7m`，新提交后旧快照仍恢复 C 的相同摘要；当前文件夹回归使用 `/tmp/chunkharbor-history-ssh-dpMoyZEZ`，同时核对中文子目录文件摘要。

测试数据库和源数据仅留在专用 `/tmp` 根，本地夹具使用 `QTemporaryDir`/`TemporaryDirectory`。日志、截图和构建产物位于忽略的 `build-windows-gui`，不纳入源码。第一次 SSH 启动曾与尚未结束的链接过程重叠，产生 WinError 32；完成构建后重跑通过，此失败不记为功能通过。

## 限制和知识点

- 范围选择是客户端候选集策略，不是服务端文件移动或删除，不改变历史逻辑身份。不包含独立空目录、源删除标记、严格源目录时间点快照。
- 精确剪枝和完整后代文件计数不可兼得。本轮报告实际检查文件及剪枝目录，明确子树数量未知。
- 映射根是路径身份锚点，扫描目录只是枚举边界。相同内容位于不同逻辑路径仍是不同文件；哈希用于完整性及分块复用，不替代文件身份。
- Qt 的控件对象归 GUI 线程，阻塞扫描和网络留在 worker；不可把 socket 跨线程直接调用。取消是协作式检查安全边界，已持久确认的块和会话保留。
- RAII 在异常路径释放源句柄；预览不是锁住未来状态，正式执行仍须再次验证源和服务器版本。
- 仍为串行传输、64 MiB 单文件限制，不增加下载续传或新协议。Linux CLI 继续原目录入口，本轮新的范围控件面向 Windows GUI。
- MSVC、真实盘满、硬件掉电、长时间大规模规则压力，以及人类逐项操作原生文件对话框未验证，不宣称通过。自动化原生窗口截图/几何和完整人工验收分别记录。
- 本轮 Windows 源符号链接用例尝试 `std::filesystem::create_symlink`，MinGW 返回 `Function not implemented`，明确 SKIP。源重解析点拒绝和 Windows 句柄复核属于保留的安全设计，不把这次跳过的夹具写成攻击运行验证通过。
