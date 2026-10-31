# M1/M2 帧与上传协议

当前实现为 `chunkharbor_protocol`，API 在 `protocol/FrameCodec.h`；不含 Linux/Qt 依赖。

M2首个子任务新增严格HELLO/HELLO_ACK与握手ERROR，运行于独立 `--handshake` 模式。字段、整数/字符串/数组限制、请求关联及状态机见 [HANDSHAKE.md](HANDSHAKE.md)；M1 Echo语义保持独立。

M2 DATA_CHUNK已接入UploadStorage实际摘要比对、受限写入和持久回执。60字节业务头的结构编解码仍独立，见 [DATA_CHUNK.md](DATA_CHUNK.md)。结构解码本身不读取磁盘。

M2 BEGIN_UPLOAD已接入会话分配和安全暂存；字段/整数精度/路径语法见 [BEGIN_UPLOAD.md](BEGIN_UPLOAD.md)。原握手NUL尾缀问题已修复。

M2 UPLOAD_PLAN返回新会话的全量块区间，不提供重启续传或增量。区间及512区间上限见 [UPLOAD_PLAN.md](UPLOAD_PLAN.md)；空计划不等于文件已提交。

M3 `QUERY_SESSION`(type7) 携带严格的 `session_id` 控制 JSON；活跃会话返回 `SESSION_STATUS`(type8)，字段为 `session_id`、`chunk_size`、`block_count`、`missing_bitmap`。位图为小写十六进制，字节内低位对应较小块索引，1 表示缺失；最大65536块、8192字节原始位图。已提交会话返回 `COMMIT_RESULT`，无效/不可恢复会话返回业务ERROR。请求 ID 仅用于帧关联，不能代替稳定会话 ID。服务端重启会核验已确认块的实际数据与数据库记录；协议格式本身不证明磁盘持久性。

M2 CHUNK_ACK在数据和最小回执持久后发送，固定durable:true契约见 [CHUNK_ACK.md](CHUNK_ACK.md)。业务模式仅在客户端HELLO提出upload且服务端实际启用时协商upload能力，不能把独立握手模式视为支持上传。

COMMIT_UPLOAD(type9)正文仅session_id；COMMIT_RESULT(type10)严格包含session_id、path、size、sha256、committed:true。上传ERROR(type15)仅code/message，支持UploadControl.h白名单。所有控制消息均拒绝重复/未知/缺失、错误类型、NUL尾缀及超限，整数不经double；上传请求ID非零且严格递增。真实流程、错误和发布窗口见 [M2_USAGE.md](M2_USAGE.md)。

固定32字节显式大端序：magic:uint32(0x43484B48)、version:uint16(1)、type:uint16、flags:uint32(0)、request_id:uint64、payload_length:uint64、reserved:uint32(0)。类型1–23识别为已定义编号；识别编号不等于支持对应业务。

控制类型payload上限64KiB；DATA_CHUNK(5)/DOWNLOAD_CHUNK(14)上限1MiB（包含其业务头）。解析到32字节头后立即检查magic/version/type/flags/reserved/长度；不根据未验证长度分配内存。正文不全返回NeedMore且consumed=0，完整则返回拥有payload的Frame和消费长度，粘包逐帧解析。M1不解析JSON、会话或块哈希。

Golden vector：type16、request_id=0x0102030405060708、payload="abc"：

```text
43 48 4b 48 00 01 00 10 00 00 00 00 01 02 03 04
05 06 07 08 00 00 00 00 00 00 00 03 00 00 00 00 61 62 63
```

Ubuntu与Windows运行相同测试逐字节验证。实际帧不使用struct内存布局，也不经浮点数处理64位字段。

`--echo`仅用于M1网络演示：PING(16)的不透明测试payload回PONG(17)；DATA_CHUNK(5)的测试payload原样回显，request_id保留。此模式不采用后续JSON控制/60字节块业务头语义，不接受BEGIN_UPLOAD等业务请求。M2必须在独立业务处理器中严格解析控制字段与块格式，不能沿用Echo作为上传确认。
