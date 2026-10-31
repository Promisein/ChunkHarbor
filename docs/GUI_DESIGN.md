# Qt 最小连接原型（M2）

提供地址、端口、连接按钮和状态提示，默认 127.0.0.1:18787。普通启动不自动连接。仅协商 handshake 能力，无上传、扫描、任务表和恢复。完整 GUI 留在 M5，上传仍使用 Linux CLI。

MainWindow 仅组装 UI，QtTcpTransport 独立持有 QTcpSocket 和单次定时器，在所属 GUI 线程异步收发。复用纯 C++ FrameCodec 和 HandshakeExchange；接收累计上限 4096 字节 payload 加 32 字节帧头，提前拒绝超限长度和同批额外数据。连接与握手总限时 5 秒，错误和断连显示状态。QObject 父子所有权管理 socket、timer 和窗口生命周期。无主线程磁盘、哈希或阻塞网络调用。

--probe-port N 使用同一窗口及正常 transport 自动连接本地服务器，成功退出 0，错误或超时退出 2，用于集成测试。--screenshot PATH 显示后保存截图并退出。临时本地假服务覆盖分片 ACK、错误 request_id、布尔类型错误、断连、超限、尾随字节和超时。实际构建验证记录见 docs/PROGRESS.md。

后续先从 Linux CLI 抽取纯 C++ 任务状态到 client_core，再添加 Qt 上传适配。M5 使用任务模型，明确区分 sent、durable confirmed 与 committed；后台扫描/哈希通过 queued signal 返回，socket 留在所属 QObject 线程。Windows 本地路径与相对 UTF-8 逻辑路径分离。Linux Reactor 和 UploadStorage 不进入 Windows 构建。开发仅使用本地或 SSH 隧道。
