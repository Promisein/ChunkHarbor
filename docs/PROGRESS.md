# ChunkHarbor 开发进度

- 当前阶段：M1 已完成（单线程 LT 网络与帧协议验收）；M2 尚未开始。
- 最近更新：2026-10-30
- 本轮范围：用户明确授权完成整个 M1，并同步虚拟机与 GitHub。

## 已完成

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

平台：Ubuntu 22.04，GCC 11.4，CMake 3.22.1，Python 3.10.12；正式目录 `/home/ning/ChunkHarbor`。

- Debug：`cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug`；`cmake --build build -j 2`；`ctest --test-dir build --output-on-failure`：**11/11通过**。
- ASan/UBSan：`cmake -S . -B build-asan -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCHUNKHARBOR_SANITIZERS=ON`；构建后设置 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1` 运行CTest：**11/11通过**。
- Release：`cmake -S . -B build-release -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release`，构建与CTest：**11/11通过**。
- Windows MinGW 13.2 / Ninja：禁用 server/CLI/GUI，构建 protocol/client_core 与协议测试：**1/1通过**。精确命令与验收对应见 `docs/M1_VALIDATION.md`。
- 原有 Buffer 的5项测试及之前事件循环的4项测试，是早期历史结果；当前以11项为准。

## 未验证与边界

- 未做性能基准、TSan、Qt GUI联调、TLS/认证或文件备份测试；不宣称吞吐量提升。
- 单loop是本轮完成的M1基线；多线程Reactor为后续扩展，不以100连接功能测试推断性能。
- M1 Echo将PING与DATA_CHUNK的正文当作不透明测试字节；尚无JSON业务解析、上传确认、文件写入、SHA-256、SQLite或重启恢复。
- 服务端只监听127.0.0.1。每连接输入上限1MiB+32，输出4MiB；这些限制不含内核缓冲、短暂副本和allocator开销。
- 所有连接注册/操作/最终销毁归属loop线程；跨线程投递调用方须保证EventLoop仍存活，队列满抛length_error。
- 旧验证目录 `/home/ning/ChunkHarbor-verify-20261030` 不作正式开发目录；原目录备份仍保留。

## 下一步（仅一项）

- [ ] M2：实现并测试 HELLO/HELLO_ACK 控制字段的严格编解码和能力协商，保持与Echo测试模式分离。
