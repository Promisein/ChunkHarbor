# M2 子任务：严格 HELLO / HELLO_ACK

本轮只实现协议握手，不实现身份认证、上传、文件或磁盘操作。Echo保留独立的 `--echo` 模式；新模式为 `--handshake`。两个参数互斥，默认仍仅监听127.0.0.1。

当前M2另有独立 `--upload --root ...` 模式，复用握手后提供文件上传；仅在该模式且客户端提出upload时返回upload能力。本文的 `--handshake` 模式仍只做握手，不因此成为上传服务。

## 字段与边界（v1）

已有指南未固定HELLO字段，本轮确定以下约定；JSON字段名区分大小写，所有列出的字段必需，不允许额外字段或重复键。

HELLO（type=1）：

```json
{"client_name":"chunkharbor-cli","versions":[1],"capabilities":["handshake"]}
```

- client_name：1–64字节可打印ASCII，不接受控制字符或NUL。它是产品标识，不是用户身份或本地路径。
- versions：1–8个互不重复的整数，范围1–65535。浮点数、指数形式、布尔值、负数、零均拒绝。
- capabilities：0–16个互不重复的标识，每个1–32字节，字符为小写ASCII字母、数字、下划线。未知能力可提出，但不能当成已支持。

HELLO_ACK（type=2）：

```json
{"capabilities":["handshake"],"max_control_payload":65536,"max_data_payload":1048576,"server_name":"chunkharbor-server","version":1}
```

- version：从客户端版本列表中选择，当前只实现1；无共同版本返回UNSUPPORTED_VERSION。
- server_name：同client_name的字符串限制。
- capabilities：同HELLO的数组限制，必须是客户端声明与服务器实际支持的交集。当前仅支持handshake；若客户端未提出它，可以返回空交集。不声明upload/resume等未实现功能。
- max_control_payload、max_data_payload：严格整数，分别在1–65536、1–1048576之内。当前服务端返回硬上限，客户端拒绝无效上限。

这三类握手消息（HELLO/ACK/本轮ERROR）额外限制正文最大4096字节；此限制小于通用FrameCodec的控制消息64KiB上限。JSON正文须完整且UTF-8有效，不接受注释、尾逗号、BOM、尾随非空白或多JSON值。限制嵌套深度，禁止嵌套对象；字段的具体数组/字符串类型再次严格验证。解析时检查重复键，避免库默认覆盖重复字段。整数从int64/uint64读取，不经过double。

2026-10-31修复：共享解析入口先使用 `string_view::find('\0')` 扫描整个有长度的消息体，拒绝任何原始NUL。原因是JSON库lexer将NUL视为输入结束，仅依赖其严格解析会漏掉NUL之后的尾随字节。三类消息均返回 `MALFORMED_MESSAGE`；JSON文本中的 `\u0000` 是转义字节序列，仍由字段规则拒绝。正常JSON前后空白和4096字节上限保持原样。扫描不复制数据，时间O(N)、额外空间O(1)，没有新增线程、共享状态或所有权要求。

回归覆盖每类消息的合法JSON后加NUL、NUL+垃圾、NUL+第二段JSON（共9例），以及前置/中间NUL、转义NUL、正常空白和长度边界。网络集成覆盖服务端拒绝恶意HELLO并发送ERROR/关闭连接，CLI拒绝恶意ACK/ERROR且报告MALFORMED_MESSAGE。修复前Windows单测报告9例被接受，Ubuntu握手单测及集成均失败，修复后完整回归通过。

## request_id、状态和错误

HELLO帧的request_id必须非零，支持完整uint64范围；ACK/ERROR保留该ID。客户端只接受当前Pending请求的关联响应，拒绝未提出的版本和能力；Pending校验成功后Ready，失败后Failed。Ready/Failed都是终态，再次accept会报错且不改变终态，不能重用同一客户端交换对象。

每条连接在创建时获得独立的处理器闭包，状态为AwaitHello → Ready。不能通过FD缓存握手状态；连接释放不会把Ready遗留给后来的连接。

| 情况 | ERROR.code |
|---|---|
| JSON/字段/类型/范围非法、request_id为0 | MALFORMED_MESSAGE |
| 无共同协议版本 | UNSUPPORTED_VERSION |
| 握手前收到非HELLO消息 | HANDSHAKE_REQUIRED |
| 成功后再次HELLO | ALREADY_HANDSHAKEN |
| 成功后发送其他操作 | UNSUPPORTED_OPERATION |

ERROR（type=15）正文只有code、message两个字符串，示例：`{"code":"UNSUPPORTED_VERSION","message":"No common protocol version"}`。code只接受上表本轮稳定值；message为1–160字节可打印ASCII，不包含原始输入、服务端路径或凭据。将来扩展错误码需要同步更新协议与测试。

业务错误先发送ERROR，再停止读取并在输出排空后关闭。无法信任的帧头（magic/version/flags等错误）直接关闭，不伪造业务错误响应。合法HELLO后的连接可以保持Ready，但本轮不处理任何上传操作。CLI完成单次握手后关闭自己的连接，尚非持久客户端会话。

## 模块与所有权

- protocol/Handshake：typed消息、严格JSON校验与序列化、版本/能力协商；没有Qt/POSIX依赖。
- client_core/HandshakeExchange：Pending/Ready/Failed、request_id关联、对所选版本/能力的复核。
- server/HandshakeHandler：每连接闭包状态，应用层处理；复用FrameCodec与Buffer，Echo处理器不变。
- TcpServer新增连接配置回调，仅用于在start前安装每连接处理器；回调在所属EventLoop执行。
- TcpConnection新增shutdownAfterWrite，错误响应可能遇EAGAIN时仍保留输出，排空后关闭。连接与回调继续遵守原owner保活和线程归属规则。
- Linux CLI复用原非阻塞socket/poll单次交换，并为握手限制响应累计容量。请求仍有5秒期限。

## 依赖与验证

使用固定版本nlohmann/json 3.11.3（MIT），单头与许可证随仓库保存，构建不需要联网或系统安装JSON包。来源与SHA-256见 `third_party/nlohmann/README.md`。

Ubuntu正式目录构建及CTest：13/13；ASan/UBSan：13/13。Windows MinGW13.2共享库与CTest：2/2。新增测试覆盖合法往返与规范序列化、类型/范围/重复键/未知字段/深度/UTF-8、无共同版本、错误关联与能力、半包/同包、32连接隔离、重复HELLO及错误响应后关闭。原M1测试继续运行。Release、TSan和Qt GUI本轮未执行；没有性能测量。

运行：服务端 `./build/chunkharbor-server --handshake --port 8787`；另一终端 `./build/chunkharbor-cli hello --port 8787 --client-name chunkharbor-cli`。预期HELLO OK、version=1、capabilities=handshake。HELLO成功不表示认证或可上传。
