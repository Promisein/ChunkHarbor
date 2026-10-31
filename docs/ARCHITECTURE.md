# 当前架构（M1/M2）

- `chunkharbor_protocol`：纯 C++17 帧编解码，不依赖 Buffer、POSIX 或 Qt；一次解析一个帧，返回 NeedMore/Complete/Error。调用方保留半包、消费已完成长度。
- `chunkharbor_client_core`：最小 Echo 请求/响应编排，复用 protocol；后续再加入上传状态机。

M2增加protocol/Handshake、UploadControl与上传消息，以及client_core/HandshakeExchange。Echo、独立握手和上传分别选择；TcpServer初始化钩子仅安装处理器。UploadHandler的逐连接状态串联HELLO到提交，一次一个异步请求。错误响应通过shutdownAfterWrite排空后关闭。
- `chunkharbor_linux_net`：UniqueFd、Buffer、epoll、Channel、EventLoop、TcpConnection、TcpServer，仅 Linux。
- `chunkharbor_server_core`：Linux安全暂存及UploadStorage，受限目录FD、块写入/同步回执、全文件校验和不覆盖发布。
- `chunkharbor_hash`：独立可移植OpenSSL EVP工具，protocol不依赖它。
- `apps/server`：配置组装、signalfd停止、三种处理器；`apps/cli`：Linux非阻塞socket/poll适配，echo/hello及文件流式上传。上传连接5秒期限、单请求30秒期限。两者无需Qt。

DiskExecutor为两个worker、64个outstanding任务，磁盘/哈希不在loop执行。应用读暂停独立于输出高低水位；完成回调发送结果后queueInLoop延后恢复读取，使当前executor完成项先释放容量再重放缓冲。worker闭包拥有请求数据/session，完成回调只弱引用连接；store保留未完成session，断连不触发在loop上的磁盘清理或丢弃已确认数据。

UploadStorage的session mutex保护块位图/摘要/写入/提交，全局mutex保护配额与同路径占用。匿名文件先安全链接为私有具名会话数据，再持久化元数据和目录项；块ACK要求数据fdatasync后追加并fdatasync回执。提交整文件校验后renameat2(NOREPLACE)并同步目录。M2保留同步追加回执而不提供SQLite/query/resume，发布中断状态可能需要M3协调，详见M2_USAGE.md。

EventLoop 绑定创建线程，Channel 借用 FD；注册、修改、移除、连接关闭和最终析构均在所属 loop。queueInLoop/quit 可跨线程调用，但调用方必须保证 loop 存活；quit 先于 run 时会使该次 run 立即退出。正常退出允许再次 run；异常回调会向上抛出，服务进程退出清理，不能用异常当业务拒绝路径。

待执行任务队列默认上限4096项，可在构造时调整；满时抛length_error。执行批次与下一批pending可能短暂并存，任务捕获对象的内存由调用方管理，不能用数量限额推断总内存。

TcpConnection 从 shared_ptr 启动后将 Channel tie 到 weak owner，派发整个期间持有强引用；回调移除注册后不继续派发本轮其他读写事件。Token 防止事件批次中 FD 移除/复用后的过期分发。stack-owned wake/accept/timer/signal Channel 由外部保证先于 loop 析构。禁止在回调中绕过 owner 直接 delete Channel。

TCP recv/send 均非阻塞，EINTR 重试，EAGAIN 等待下一事件；MSG_NOSIGNAL 避免 SIGPIPE。按需开启 EPOLLOUT，写完取消监听。输入有界，输出高水位暂停读、低水位恢复；最大输出超限关闭连接。半关闭后，完整请求生成的待发送响应排空后关闭；残缺帧 EOF、协议错误直接关闭。每个回调 I/O 预算 256 KiB；accept 每轮最多64次。连接默认空闲30秒、最多512个；timerfd 每100ms检查空闲，accept 资源不足暂时移除监听，下一计时重试。

关闭顺序：停止 accept/timer → 在 loop 线程关闭全部连接并从 map 移除 → Channel 移除 → FD 释放 → EventLoop 释放。连接关闭回调可移除 map 所有权，当前派发保活引用确保安全返回。

Windows连接原型已经通过QtTcpTransport/QTcpSocket适配client_core/protocol；当前上传编排仍位于Linux CLI，完整GUI上传需后续按接口抽取。MainWindow只组装窗口，socket/timer在所属QObject线程异步使用。服务端不链接Qt。SQLite与重启恢复留M3；现有单loop网络框架保留。
