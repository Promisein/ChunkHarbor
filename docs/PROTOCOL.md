# 协议状态

协议尚未实现。M1 将按 [开发指南](../CODEX_DEVELOPMENT_GUIDE.md) 的第 5 节实现版本 1 的 32 字节网络字节序帧头和增量解码。M2 开始实现上传消息；当前两个可执行程序不会建立连接。
新版指南要求协议实现为 Linux/Windows 共用的纯 C++17 target，使用两端相同的 golden vectors 验证字节序、长度和 64 位字段。Qt 的 `QTcpSocket` 只作为传输适配层，不参与协议编码；这些能力目前均未实现。
