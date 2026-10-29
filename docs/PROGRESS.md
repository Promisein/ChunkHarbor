# ChunkHarbor 开发进度

- 当前阶段：M1（第一子任务完成，阶段未全部完成）
- 最近更新：2026-10-30

## 已完成

- [x] CMake C++17 基础工程、核心库 target、服务端与客户端 `--help`。
- [x] `UniqueFd`、基础错误对象和同步日志接口。
- [x] CTest 基础测试、配置样例、格式配置与基础文档。
- [x] M1：单线程 LT EventLoop、Channel、EpollPoller、eventfd 跨线程唤醒与基础事件测试。

## 正在进行

- [ ] M1 后续：Buffer、TcpConnection、FrameCodec 与 TCP 网络集成测试。
- [ ] 按新版 Windows Qt + Linux Reactor 指南逐步建立平台 target 边界；当前 `chunkharbor_core` 仍混合 Linux `net`，没有可移植协议库。

## 下一步

- [ ] M1：实现独立 Buffer，覆盖追加、消费、边界与容量限制测试。

## 本轮测试证据

- 2026-10-30 M1 审计：源码包含 `EventLoop`、`EpollPoller`、`Channel` 及其单元测试；缺少 `Buffer`、`TcpConnection`、`TcpServer`、跨平台 `FrameCodec`、Echo/握手模式及 100 连接功能测试，**M1 未完成**。本轮虚拟机 SSH 连接超时，未重新运行 Ubuntu 构建和 CTest；上次 4/4 结果只代表当时的源码。GitHub CI 配置用于后续独立验证 Linux 构建。
- GitHub [Linux CI #1](https://github.com/Promisein/ChunkHarbor/actions/runs/40912229775) 对首次提交 `7bca78c` 显示 Success；该工作流执行 Ubuntu 22.04 上的 CMake 配置、构建和 CTest。公开页面未显示逐项测试日志，不能从页面确认单测数量。
- 2026-10-30 文档核对：Windows 目录中的 `AGENTS.md` 和 `CODEX_DEVELOPMENT_GUIDE.md` 已更新为 M0–M7、Windows Qt GUI 路线；本轮仅修改文档，未重新构建或执行测试。SSH 到 `miniob-ubuntu` 两次连接超时，虚拟机副本尚未同步。

- `cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug`：通过（Ubuntu 22.04，GCC 11.4.0，CMake 3.22.1）。
- `cmake --build build -j 2`：通过。
- `ctest --test-dir build --output-on-failure`：3/3 通过（common_test、server_help、cli_help）。
- `./build/chunkharbor-server --help`、`./build/chunkharbor-cli --help`：均退出成功并显示尚未实现的功能。
- 本轮 `cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug`、`cmake --build build -j 2`：通过。
- 本轮 `ctest --test-dir build --output-on-failure`：4/4 通过，新增 `eventloop_test` 覆盖管道可读事件、跨线程任务投递、移除监听与退出。

## 已知问题与设计决策

- 当前无网络监听、配置解析、文件传输或数据库功能；示例配置仅作后续阶段约定。
- Ubuntu 虚拟机未安装 Ninja，本轮使用 Unix Makefiles 生成器。
- 尚未进行网络集成测试、故障恢复测试或性能测试，因为对应模块尚未实现。
- M1 尚未实现 TCP 服务、客户端连接或帧解析；事件循环仅可通过测试程序验证。Channel 回调期间由 FD 所有者保证其生命周期。
- 新版指南的 M0 跨平台 target 选项和纯 C++17 协议库尚未实现；README 保留当前真实可用的 Linux 构建命令。Windows Qt GUI 尚无源码或验证结果。
