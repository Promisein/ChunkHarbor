# 当前架构（M1）

- `chunkharbor_protocol`：纯 C++17 帧编解码，不依赖 Buffer、POSIX 或 Qt；一次解析一个帧，返回 NeedMore/Complete/Error。调用方保留半包、消费已完成长度。
- `chunkharbor_client_core`：最小 Echo 请求/响应编排，复用 protocol；后续再加入上传状态机。
- `chunkharbor_linux_net`：UniqueFd、Buffer、epoll、Channel、EventLoop、TcpConnection、TcpServer，仅 Linux。
- `apps/server`：配置组装、signalfd 停止、Echo 测试处理器。`apps/cli`：Linux 非阻塞 socket/poll 适配、五秒请求期限。两者均无需 Qt。

EventLoop 绑定创建线程，Channel 借用 FD；注册、修改、移除、连接关闭和最终析构均在所属 loop。queueInLoop/quit 可跨线程调用，但调用方必须保证 loop 存活；quit 先于 run 时会使该次 run 立即退出。正常退出允许再次 run；异常回调会向上抛出，服务进程退出清理，不能用异常当业务拒绝路径。

待执行任务队列默认上限4096项，可在构造时调整；满时抛length_error。执行批次与下一批pending可能短暂并存，任务捕获对象的内存由调用方管理，不能用数量限额推断总内存。

TcpConnection 从 shared_ptr 启动后将 Channel tie 到 weak owner，派发整个期间持有强引用；回调移除注册后不继续派发本轮其他读写事件。Token 防止事件批次中 FD 移除/复用后的过期分发。stack-owned wake/accept/timer/signal Channel 由外部保证先于 loop 析构。禁止在回调中绕过 owner 直接 delete Channel。

TCP recv/send 均非阻塞，EINTR 重试，EAGAIN 等待下一事件；MSG_NOSIGNAL 避免 SIGPIPE。按需开启 EPOLLOUT，写完取消监听。输入有界，输出高水位暂停读、低水位恢复；最大输出超限关闭连接。半关闭后，完整请求生成的待发送响应排空后关闭；残缺帧 EOF、协议错误直接关闭。每个回调 I/O 预算 256 KiB；accept 每轮最多64次。连接默认空闲30秒、最多512个；timerfd 每100ms检查空闲，accept 资源不足暂时移除监听，下一计时重试。

关闭顺序：停止 accept/timer → 在 loop 线程关闭全部连接并从 map 移除 → Channel 移除 → FD 释放 → EventLoop 释放。连接关闭回调可移除 map 所有权，当前派发保活引用确保安全返回。

Windows GUI 将仅链接 client_core/protocol，通过 QTcpSocket 适配；Linux CLI 通过 POSIX/poll 适配。服务端不链接 Qt。server_core/存储/有界磁盘执行器将在 M2 引入，当前事件线程不执行文件或数据库任务。
