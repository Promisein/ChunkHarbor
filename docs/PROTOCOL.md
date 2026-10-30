# M1 帧协议

当前实现为 `chunkharbor_protocol`，API 在 `protocol/FrameCodec.h`；不含 Linux/Qt 依赖。

固定32字节显式大端序：magic:uint32(0x43484B48)、version:uint16(1)、type:uint16、flags:uint32(0)、request_id:uint64、payload_length:uint64、reserved:uint32(0)。类型1–23识别为已定义编号；识别编号不等于支持对应业务。

控制类型payload上限64KiB；DATA_CHUNK(5)/DOWNLOAD_CHUNK(14)上限1MiB（包含其业务头）。解析到32字节头后立即检查magic/version/type/flags/reserved/长度；不根据未验证长度分配内存。正文不全返回NeedMore且consumed=0，完整则返回拥有payload的Frame和消费长度，粘包逐帧解析。M1不解析JSON、会话或块哈希。

Golden vector：type16、request_id=0x0102030405060708、payload="abc"：

```text
43 48 4b 48 00 01 00 10 00 00 00 00 01 02 03 04
05 06 07 08 00 00 00 00 00 00 00 03 00 00 00 00 61 62 63
```

Ubuntu与Windows运行相同测试逐字节验证。实际帧不使用struct内存布局，也不经浮点数处理64位字段。

`--echo`仅用于M1网络演示：PING(16)的不透明测试payload回PONG(17)；DATA_CHUNK(5)的测试payload原样回显，request_id保留。此模式不采用后续JSON控制/60字节块业务头语义，不接受BEGIN_UPLOAD等业务请求。M2必须在独立业务处理器中严格解析控制字段与块格式，不能沿用Echo作为上传确认。
