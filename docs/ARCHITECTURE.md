# 架构

当前有 `common` 和 `net` 基础组件，以及 `apps/server`、`apps/cli` 两个入口。`UniqueFd` 独占 Linux FD 并在析构时关闭；日志写入标准错误流。

M1 第一个子任务实现了单线程 LT 模式的 `EventLoop`、`Channel`、`EpollPoller`：

- `EventLoop` 绑定创建线程，只有该线程可注册、修改和移除 Channel；其他线程通过 `queueInLoop()` 投递任务，`eventfd` 唤醒 `epoll_wait()`。`quit()` 可跨线程调用。
- `Channel` 借用 FD，不负责关闭；FD 所有者必须保证 Channel 在回调结束前保持存活，并在 EventLoop 析构前移除。析构时若仍注册，会自动移除。
- `EpollPoller` 用 `EPOLL_CTL_ADD/MOD/DEL` 维护注册；禁用全部事件会移除注册，重新启用会重新添加。每次注册使用新的 token，避免同一次事件批次中移除和复用 FD 后分发过期事件。`epoll_wait()` 遇 `EINTR` 重试。LT 模式下，后续 socket 读取和写入仍需处理 `EAGAIN`、`EINTR` 与部分发送。

计划中的完整分层为 `net`、`protocol`、`transfer`、`storage`、`concurrency`、`common`。`Buffer`、`TcpConnection`、`FrameCodec` 和 TCP 集成测试尚未实现；磁盘、哈希、数据库工作将在后续阶段交给有界工作池。

## 新版跨平台目标（尚未实现）

- Ubuntu 服务端保留自主 `epoll` Reactor，不依赖 Qt；Windows Qt Widgets GUI 使用 `QTcpSocket`，不编译 Linux `net`、`UniqueFd` 等系统调用代码。
- 计划把纯 C++17 的 `chunkharbor_protocol` 与 `chunkharbor_client_core` 作为 Linux CLI 和 Windows GUI 的共享层。协议显式编码大端字节序，并用两端一致的 golden vectors 验证；Qt 类型和 POSIX 头文件不能进入共享协议库。
- GUI 的主线程负责展示和非阻塞 Socket 事件；目录扫描、哈希和大文件读写由工作线程处理，结果投递回对象所属线程。GUI 进度以服务端确认和提交结果为准。
- 当前 CMake 只有 `chunkharbor_core`，混合了 `common` 和 Linux `net`；尚未拆出可跨平台 target 或加入 `CHUNKHARBOR_BUILD_*` 选项。这是后续按新版指南推进时要处理的结构缺口，不代表 M0/M1 已满足新版跨平台验收。
