# ChunkHarbor

后续开发需求已冻结，采用 [R1–R8 路线图](docs/ROADMAP_V3.md)：单用户可靠性 → 单服务器多用户隔离 → 性能与规模验证。M0–M7 保留为原始历史编号。当前源码、跨机器版本及验证边界见 [2026-11-03 审计](docs/PROJECT_AUDIT_2026-11-03.md)，每轮状态见 [PROGRESS 顶部](docs/PROGRESS.md)。

R1已于2026-11-03按用户确认完成，当前进入R1发布与跨平台同步；R2尚未启动。最新提交、同步和验证记录见 [PROGRESS](docs/PROGRESS.md) 与 [Ubuntu同步记录](docs/UBUNTU_SYNC.md)。

C++17 单向增量备份项目。**M1–M4后端已实现；M5 Windows Qt Widgets客户端已接入真实目录预览、增量备份和持久续传。** 自主Linux Reactor、SQLite会话与固定块增量支持CLI和GUI；Windows经SSH真实备份及服务端SIGKILL后恢复已验证。M6已支持Linux CLI单文件下载校验恢复；GUI恢复、TLS/认证未实现。单文件上限64MiB，仅用于可信loopback/SSH开发。

M4使用、详细人工复现及自动恢复测试见[M4使用](docs/M4_USAGE.md)；真实构建/测试证据见[M4验证](docs/M4_VALIDATION.md)。旧`upload`命令保持首次上传/已有目标拒绝的语义；重复增量备份使用`backup`。版本对象与最新副本分别存储，尚无历史版本GC或总空间配额。

Windows中文GUI提供总览、备份、文件、版本、日志、设置六页：后台扫描/哈希、服务端差异预览、真实确认进度、暂停/取消/恢复、本机提交历史、日志筛选导出和设置持久化。文件页已接入服务端当前文件分页、翻页/取消及当前页筛选；版本历史页仍为本机提交记录，恢复按钮禁用。构建和详细操作见[Windows使用](docs/BUILD_WINDOWS.md)，线程与模块结构见[GUI设计](docs/GUI_DESIGN.md)，验收见[M5验证](docs/M5_VALIDATION.md)和[GUI列表验证](docs/M6_GUI_FILES_VALIDATION.md)。

## R1：可靠备份与自动文件计划

普通备份流程：选择本地目录与远端相对路径 → 使用“自动备份（推荐）”扫描 → 检查每个文件的执行策略和预计传输量 → 确认开始。新用户默认auto，已有手动偏好保留。

| 模式 | 行为 |
| --- | --- |
| auto（推荐） | 每个文件独立选择首次上传、增量、有效会话续传或跳过；无法安全判断的文件标错，完整扫描后可确认执行其他安全文件 |
| full | 首次上传，拒绝覆盖已有目标；本任务已完成且内容一致的文件可核验后跳过 |
| resume | 严格核验本机检查点及服务端会话，无有效会话不静默开始新上传 |
| incremental | 新文件上传，已有文件按固定索引块复用；无变化跳过；按设置复用匹配未完成会话 |

执行前复核源SHA-256、服务器固定版本及续传会话；配置变化使预览失效。部分失败后“重试 / 恢复”先重新扫描，确认后再执行。损坏/不匹配的检查点保留，不自动删除状态；无变化不创建重复版本。详见 [自动备份设计与验证](docs/R1_AUTO_BACKUP.md)。

R1同时修复了GUI健康探测连接正常关闭造成的离线误判、服务端磁盘工作期间误判空闲导致上传断开，以及下拉框圆角外的矩形底框。健康徽标仅表示最近HELLO可达，不代替上传成功或持久确认。

验证采用Windows Qt构建、业务回归与真实Windows→SSH→Ubuntu上传/重启恢复矩阵。下拉框已由用户人工确认；原生自动输入测试的失败记录和人工验收例外保留，不宣称所有自动化全绿。单文件仍限制64MiB，仅可信loopback/SSH使用；GUI下载、服务端历史查询和目录快照归R2，TLS/认证及性能扩展按后续路线推进。

## 增量备份快速开始

M6服务端支持`file_list_v1`受限分页文件列表（最多64条/64KiB），Windows GUI已接入，每页请求32条；CLI尚无列表命令。Linux CLI新增`download --path LOGICAL --output LOCAL`，临时下载、逐块/整文件SHA-256校验后原子发布，拒绝覆盖已有文件。下载协议见[M6下载](docs/M6_DOWNLOAD.md)，用法及双平台验收见[下载验证](docs/M6_DOWNLOAD_VALIDATION.md)；列表协议及历史验收见[M6文件列表](docs/M6_FILES_LIST.md)、[列表验证](docs/M6_VALIDATION.md)。

启动下方 Ubuntu 服务端后，在另一终端对专用测试文件执行：

```bash
work=$(mktemp -d /tmp/chunkharbor-cli-XXXXXXXX)
printf 'first backup\n' > "$work/source.txt"
./build/chunkharbor-cli preview --file "$work/source.txt" --path demo/source.txt --state "$work/session.state"
./build/chunkharbor-cli backup --file "$work/source.txt" --path demo/source.txt --state "$work/session.state"
./build/chunkharbor-cli backup --file "$work/source.txt" --path demo/source.txt --state "$work/session.state"
```

第二次备份预期 `blocks_sent_this_run=0`。Windows CLI、SSH 隧道、目录备份与中断续传步骤见 [M4 使用指南](docs/M4_USAGE.md)。

## Ubuntu 构建与验证

Windows与Ubuntu使用同一完整Git仓库。Ubuntu保留`apps/gui`源码，通过`-DCHUNKHARBOR_BUILD_GUI=OFF`关闭GUI目标，无需Qt；Windows关闭SERVER、开启GUI。构建目录、SDK、测试照片和备份数据留在各自机器，不上传Git。Linux日常入口仍为原工程`/home/ning/ChunkHarbor`，复用原build及依赖；跨机同步使用Git提交和快进更新，先保存并核验目标工作区，不直接覆盖源码。发布轮真实构建/测试结果见PROGRESS顶部，旧M6结果保留在[Ubuntu同步记录](docs/UBUNTU_SYNC.md)中，不当作R1新测试。

在 Ubuntu 的仓库根目录构建，无需 Qt。依赖 CMake 3.20+、GCC、Make、Python 3、OpenSSL 与 SQLite 开发包（Ubuntu `libssl-dev libsqlite3-dev`）；GDB 为可选调试工具。

```bash
cd /path/to/ChunkHarbor
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 2
ctest --test-dir build --output-on-failure
```

M3自动验证包含SQLite事务、重启查询、损坏暂存块、数据库写入故障和Windows CLI经SSH的上传与续传。实际测试命令、结果及人工验收步骤见 [M3 验收](docs/M3_VALIDATION.md)；历史结果见 [M2 验收](docs/M2_VALIDATION.md)。M1集成默认100连接，不以功能测试推断吞吐量。

## 运行文件上传

Ubuntu终端一（使用专用新目录，存在则自行确认用途和权限）：

```bash
mkdir -m 700 /tmp/chunkharbor-m2-data
./build/chunkharbor-server --upload --root /tmp/chunkharbor-m2-data --port 8787
```

Ubuntu终端二：

```bash
cd /path/to/ChunkHarbor
source_file=$(mktemp /tmp/chunkharbor-demo-XXXXXX)
printf 'ChunkHarbor M2 upload\n' > "$source_file"
./build/chunkharbor-cli upload --file "$source_file" --path examples/demo.txt --port 8787
sha256sum "$source_file" /tmp/chunkharbor-m2-data/files/examples/demo.txt
ctest --test-dir build -R 'upload_|application_pause' -V
```

预期 `UPLOAD OK`，两项摘要一致。目标已存在会拒绝覆盖；测试重跑应换逻辑路径。断连的未完成会话可通过查询恢复；Windows CLI 的会话保存和重启续传步骤见 [M3 验收](docs/M3_VALIDATION.md)。未完成会话在30天后于服务端启动时过期并清理暂存文件。

## 运行 HELLO 握手

Ubuntu终端一：`./build/chunkharbor-server --handshake --port 8787`。

另一个Ubuntu终端：`./build/chunkharbor-cli hello --port 8787 --client-name chunkharbor-cli`。

预期输出 `HELLO OK: version=1 server=chunkharbor-server capabilities=handshake ...`。握手仅协商版本和实际能力，**不代表认证或支持文件上传**。`--handshake` 与 `--echo` 互斥，完整字段、错误码与状态规则见 [握手契约](docs/HANDSHAKE.md)。握手测试可运行 `ctest --test-dir build -R handshake -V`。

## 运行 Echo

Ubuntu 终端一：

```bash
./build/chunkharbor-server --echo --port 8787
```

看到 `LISTENING 127.0.0.1:8787` 后，在另一个 Ubuntu 终端执行：

```bash
cd /path/to/ChunkHarbor
./build/chunkharbor-cli echo --host 127.0.0.1 --port 8787 --message "hello ChunkHarbor"
```

期望：`ECHO OK: hello ChunkHarbor`。服务端 Ctrl+C 或 SIGTERM 正常关闭；支持 `--idle-timeout-ms 30000`、`--max-connections 512`。`--port 0` 由系统分配端口并打印，供自动测试使用。

Echo 是独立测试模式：PING(16) 回 PONG(17)，DATA_CHUNK(5) 原样回显以测试大二进制帧；这些 payload 暂作不透明测试字节，不执行 JSON/文件块业务。其他合法但未实现的消息、非法头或超限长度会关闭连接；没有 HELLO/上传/认证语义。配置 JSON 仍只是样例，程序目前通过上述参数配置。

服务端只绑定loopback；Windows可用 `ssh -N -L 18787:127.0.0.1:8787 miniob-ubuntu` 建立隧道，再使用Qt GUI连接18787进行HELLO检测（服务端选handshake或upload模式）。真实GUI备份须使用upload模式，操作见[Windows使用](docs/BUILD_WINDOWS.md)。普通文本客户端不能替代帧客户端；连接检测不代表备份任务已连接或成功。

## 自己调试

```bash
ctest --test-dir build -R network_integration -V
ctest --test-dir build -R '^buffer_test$' -V
gdb --args ./build/chunkharbor-server --echo --port 8787
```

GDB 中可设 `break chunkharbor::TcpConnection::readReady`、`break chunkharbor::TcpConnection::writeReady`、`break chunkharbor::protocol::decode`，输入 `run` 后从另一终端运行 CLI。`bt` 看调用栈，`print *this` 看连接/缓冲状态，`next` 单步，`continue` 继续。测试二进制也可直接用 GDB 启动；连接默认有空闲超时，断点停留很久时可把 `--idle-timeout-ms` 调大。

```bash
cmake -S . -B build-asan -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCHUNKHARBOR_SANITIZERS=ON
cmake --build build-asan -j 2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-asan --output-on-failure
```

## Windows 共享协议测试

使用兼容 C++17 的编译器和 Ninja（加入 PATH），无需 Qt。Windows 默认关闭 Linux server/CLI。Qt Windows CLI 的构建与使用见 [M3 验收](docs/M3_VALIDATION.md)。以下只构建纯C++协议与client_core；当前Qt GUI真实备份的构建及SSH连接方法见[Windows使用](docs/BUILD_WINDOWS.md)，M2_USAGE仅保留原型阶段历史：

```powershell
cmake -S . -B build-windows-protocol -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCHUNKHARBOR_BUILD_SERVER=OFF -DCHUNKHARBOR_BUILD_CLI=OFF -DCHUNKHARBOR_BUILD_GUI=OFF
cmake --build build-windows-protocol
ctest --test-dir build-windows-protocol --output-on-failure
```

本机MinGW13.2共享测试及Qt6.8.3匹配Kit连接原型均已运行；Windows Qt经SSH转发到Ubuntu的真实握手已通过。后续真实GUI上传验收见M5_VALIDATION.md；MSVC和正式分发打包未验证。

## 实现边界

已拆分 `chunkharbor_protocol`、最小 `chunkharbor_client_core`、`chunkharbor_linux_net`。网络采用单线程 LT；多线程 Reactor 为后续可选扩展。每连接输入上限 1 MiB+32，输出上限 4 MiB，高/低水位 1 MiB/512 KiB；积压暂停读，降水位恢复读；每次事件收发有预算，连接数与空闲期有上限。容量限额不包括内核 socket 内存及短暂编码/扩容副本。

未实现：GUI下载恢复、TLS/认证。Linux CLI下载不提供覆盖或下载续传。M4使用schema v2、不可变版本、固定基线和条件发布；文件系统与SQLite之间的发布窗口通过启动核验协调，不能视为跨存储原子事务。新库不能交给旧二进制；真实旧备份根升级前应先备份并审核。旧M2无数据库会话仍隔离。完整规格见[指南](CODEX_DEVELOPMENT_GUIDE.md)，当前结果见[进度](docs/PROGRESS.md)、[M4验证](docs/M4_VALIDATION.md)。
