# AGENTS.md — ChunkHarbor Codex 仓库指令

> 本文件位于 **仓库根目录**。完整设计、协议、里程碑和验收条件见 `CODEX_DEVELOPMENT_GUIDE.md`。这两份文件共同约束本仓库的开发；实际实现状态以源码、测试和 `docs/PROGRESS.md` 为准。本文是要求与计划，不代表功能已实现。**用户已表示至少完成一个开发阶段；Codex 必须先审查现有代码，不允许据此推断具体实现程度或重做 M0。**

## 1. 项目定位（必须理解）

ChunkHarbor 是 **C++17 跨平台单向增量文件备份系统**：

- **Ubuntu/Linux 服务端**：自主实现非阻塞 TCP + `epoll` / Reactor，支持并发连接、分块存储、SQLite 持久化、断点续传、故障恢复和性能分析。服务端不依赖 Qt。
- **Windows 11 主客户端**：Qt 6 Widgets + Qt Network（`QTcpSocket`）图形界面，提供目录选择、备份任务、进度、连接状态、错误信息以及后期下载恢复。GUI 不使用 Linux `epoll`。
- **Linux CLI 客户端**：无 GUI 的调试 / 自动化入口，用于先验证协议与备份功能。
- **共享跨平台代码**：`chunkharbor_protocol`、`chunkharbor_client_core` 是纯 C++17；不得依赖 Qt、Linux 系统调用或编译器特定二进制结构体布局。
- **第一版范围**：客户端 → 服务器**单向备份** + 后期恢复下载；不实现双向实时同步、冲突自动合并、云盘账号系统、P2P 或多节点共识。

**核心目标是技术深度和真实可用性，而不是堆砌功能。** 服务端网络框架必须自己实现，不得直接以 Muduo、Boost.Asio、Drogon 替代自主 Reactor；可以学习它们的设计但不得直接抄代码。

## 2. 每轮 Codex 工作流程

1. 开始时读取 `AGENTS.md`、`CODEX_DEVELOPMENT_GUIDE.md`、`docs/PROGRESS.md`（若存在），检查 `README.md`、`git status`、构建环境、相关源码与测试。
2. 只选择当前 M0–M7 里程碑中的**一个最小、可测试的子任务**。用户已报告第一阶段完成：先检查 Git 状态、源码、构建和测试证据确认进度，不得重置/重建现有工程。除非用户明确授权，不要一次实现多个里程碑，也不要对已完成部分无理由大规模重构。
3. 修改前简述计划：目标、涉及文件、类接口、线程/资源所有权、异常路径、测试与跨平台影响。
4. 编码后执行该环境可运行的配置、构建、单测与相关集成测试；若环境不足，明确记录**未执行**，绝不能编造 Windows / Linux 的通过结果。
5. 同步维护 `docs/PROGRESS.md`：阶段、完成项、真实命令与结果、未验证项、风险、下一项最小任务。
6. 汇报使用**简体中文**，说明关键 C++ 知识点和重要设计取舍；代码标识符使用清晰、一致的英文。
7. 不自动 `git push`，不执行不可逆的广泛删除，不覆盖用户已有备份数据，不将测试脚本指向用户真实目录。

## 3. 不可违反的架构边界

- CMake targets：`chunkharbor_protocol`（Windows/Linux）、`chunkharbor_client_core`（Windows/Linux）、`chunkharbor_linux_net`（Linux）、`chunkharbor_server_core`（Linux）、`chunkharbor-server`（Linux）、`chunkharbor-cli`（Linux）、`chunkharbor-gui`（Windows，具备 Qt 的 Linux 可选）。按阶段创建，名称保持一致。
- `protocol` 完全脱离 Qt / POSIX Socket，采用固定宽度整数、显式大端序、确定性字节帧格式；不得发送 C++ struct 原始内存布局或把 TCP 当作消息边界。
- Linux `EventLoop` 只处理网络 I/O / 事件派发；磁盘哈希、`pwrite`、`fsync` 和 SQLite 事务交由**有界工作池**，完成后安全投递回所属 loop。
- Qt `QTcpSocket` 在其 QObject 所属线程使用，不允许工作线程直接读写它；GUI 线程不得直接执行大文件扫描、哈希与阻塞网络/磁盘操作。跨线程使用 queued signal / 合法投递机制。
- 业务层与 GUI 分离：`MainWindow` 只负责 UI 组装；数据由 `QAbstractTableModel` 等模型承载，`QtTcpTransport` 与 `TransferController` 处理网络与任务状态。
- 资源使用 RAII，特别注意 FD、sqlite 句柄、临时文件、线程、QObject 生命周期和跨线程回调的引用有效性。
- 网络解析要正确处理半包、粘包、部分写、`EINTR`、`EAGAIN`、半关闭、超限 payload；有界接收/发送缓冲和队列，慢连接需背压。

## 4. 数据一致性与安全底线

- **ACK 持久性**：只有在数据块校验、写入、约定的持久化步骤及元数据提交完成后，才能发表示“持久确认”的 `CHUNK_ACK`；具体协议语义见开发指南。
- **原子发布不等于跨存储原子事务**：暂存文件校验 → `fsync` → 安全 `rename` / 目录 `fsync` → 数据库状态协调；崩溃窗口必须通过启动修复处理。
- **断点续传**：必须有服务端会话持久化、客户端持久会话关联和重启后查询缺失块；仅“网络重连”不算断点续传。
- **固定块增量不等于 rsync 算法**：固定索引哈希比较与 rolling checksum 不可混称；任何优化需要真实对照实验。
- 文件写入必须限制到服务端配置的备份根目录；拒绝 `..`、绝对路径、符号链接逃逸与目录竞态。不能靠字符串前缀检查确保安全。
- GUI 从 Windows 文件系统读取的本地路径与发给服务器的**相对 UTF-8 逻辑路径**严格分离；恢复下载不得利用恶意目标路径覆盖本地任意文件。JSON 的 64 位文件大小/纳秒时间戳不得经浮点值处理而丢失精度。
- 服务端初期默认监听 `127.0.0.1:8787`；Windows 通过 SSH 本地转发进行安全的开发连接。未完成 TLS + 鉴权/授权不得暴露到不可信网络。
- 不记录凭据、令牌或用户文件原文到日志，不把真实数据、私钥、构建产物上传到 Git。

## 5. 里程碑（严格按顺序增量推进）

| 阶段 | 核心交付 | 测试重点 |
|---|---|---|
| **M0** | CMake、Linux server/CLI 骨架、纯 C++ protocol、平台 target 边界 | Ubuntu 编译、CTest、`--help` |
| **M1** | 自主 epoll Reactor、Buffer、TCP 连接、FrameCodec | 半包、粘包、异常断连、100 连接功能测试 |
| **M2** | 首次完整文件上传、块/整文件 SHA-256、临时发布；验证基本传输后可启动 Qt GUI 的架构文档/最小骨架 | 文件哈希一致、非法输入拒绝；GUI 骨架不宣称可上传 |
| **M3** | SQLite、持久会话、断点续传、崩溃恢复 | kill -9 / 重连 / 重复提交 |
| **M4** | 固定块增量、分页 manifest、目录批任务与备份差异预览接口 | 无变化 0 DATA_CHUNK、单块变化、预览与实际计划一致 |
| **M5** | Windows Qt GUI、QtTcpTransport、任务模型、差异预览页、端到端集成 | Windows 构建 + GUI → Ubuntu 上传与预览 |
| **M6** | 下载恢复、并发、背压、性能实验 | 文件校验、资源上界、可复现基准 |
| **M7** | TLS/认证、部署、打包、安全回归和可选研究 | 未授权访问、恶意路径、打包验证 |

**若当前是空仓库：首轮只实现 M0；若已开发一阶段，先做现状审计，再继续下一个未完成的最小子任务。** M0 预留 Qt 边界，M2 基本传输验证后可以增加非阻塞 Qt GUI 原型，M5 再完成 GUI 功能与端到端集成；GUI 不得替代 Linux Reactor。

## 6. 产品功能优先级（不可越级许诺）

- **P0 核心**：正确上传、块/整文件校验、断点续传、跨平台 GUI、备份差异预览、下载恢复；优先保证数据正确与安全路径。
- **P1 实用**：备份排除规则（如 `.git/`、`build/`）、可浏览的历史版本/快照、定时备份、用户可理解的异常提醒。在可靠基础能力完成后逐项选择；快照及其保留/回收策略需单独的设计与测试。
- **P2 研究**：自适应并发、智能块大小、传输限速与性能优化；必须提供基准对照，不应先于可靠性投入。
- 安全范围明确：首次开发仅可信本地或 SSH 隧道；未有认证与传输加密时，不把普通 TCP 端口暴露给局域网/公网。

## 7. 参考构建命令

Linux（M0 起）：

```bash
cmake -S . -B build/linux-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCHUNKHARBOR_BUILD_SERVER=ON \
  -DCHUNKHARBOR_BUILD_CLI=ON \
  -DCHUNKHARBOR_BUILD_GUI=OFF \
  -DCHUNKHARBOR_BUILD_TESTS=ON
cmake --build build/linux-debug -j 2
ctest --test-dir build/linux-debug --output-on-failure
```

Windows GUI（M2 基础传输通过后可构建原型；M5 集成正式功能，Qt Kit 以实际环境为准）：

```powershell
cmake -S . -B build/windows-gui -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCHUNKHARBOR_BUILD_SERVER=OFF `
  -DCHUNKHARBOR_BUILD_CLI=OFF `
  -DCHUNKHARBOR_BUILD_GUI=ON `
  -DCHUNKHARBOR_BUILD_TESTS=ON `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/mingw_64"
cmake --build build/windows-gui
ctest --test-dir build/windows-gui --output-on-failure
```

若 CMake 目标或构建命令尚未创建，应先完成对应阶段；实际可执行文件路径以工程为准。未经运行的命令不得标记为通过。

## 8. 每轮交付格式

```text
本轮里程碑 / 子任务：
完成的功能与关键设计：
新增或修改的文件：
实际执行的平台、编译与测试命令 / 结果：
未执行或失败的验证（必须说清原因）：
已知问题、安全风险与兼容性影响：
docs/PROGRESS.md 是否已更新：
下一轮最小任务建议：
```

**提醒**：设计规格不是已经完成的功能，README、演示视频和简历只写实际可运行且有证据的内容。
