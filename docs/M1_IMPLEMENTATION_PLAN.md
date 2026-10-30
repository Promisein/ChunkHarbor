# M1 完成计划

本轮用户明确授权完成整个 M1 并同步虚拟机/GitHub，保留现有 Buffer 和 Reactor。

1. Channel 增加 weak owner 绑定和派发期间强引用；移除后跳过其他回调。修复 eventfd EINTR 和 quit 先于 run 的语义。测试 owner 自释放和过期、移除、多事件与唤醒。
2. 纯 C++17 chunkharbor_protocol：32 字节显式大端帧，单帧增量解析，按类型限制长度，golden vectors 与半包/粘包/畸形头测试；协议不依赖 Buffer。
3. TcpConnection：nonblocking recv/send、EINTR/EAGAIN、部分发送、按需 EPOLLOUT、有界输入/输出、高低水位、owner 保活、半关闭后排空发送。TcpServer 单 loop accept，连接数量上限、idle timer、资源不足暂停 accept、受控停止。
4. Echo 演示：仅 loopback，PING→PONG 与 DATA_CHUNK 测试回显，拒绝其他业务请求；CLI 非阻塞连接及有期限的收发。上传业务仍不支持。
5. CMake 分离 protocol/linux_net/portable client_core；Windows 只构建共享协议测试，不引入 Linux/Qt。Linux CTest 加入网络集成、100 并发、快速连断、慢读、半关闭、非法长度、FD 回收；socketpair 强制短写和 EAGAIN，注入 EINTR。
6. Ubuntu Debug、ASan/UBSan、Release；独立代码审查；更新架构/协议/进度/README，提交推送并核对 GitHub 与虚拟机提交号。

关闭约束：所有 Channel/连接注册与销毁在所属 loop；queueInLoop 的调用方必须保证 loop 仍存活。发布前所有重要审查问题必须修复。
