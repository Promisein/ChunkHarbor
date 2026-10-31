# DATA_CHUNK 消息体（M2 独立编解码）

实现位于 `protocol/DataChunk.h` / `src/protocol/DataChunk.cpp`，属于纯 C++17 的 `chunkharbor_protocol`。不依赖 Qt、POSIX、OpenSSL，不操作网络、文件或会话状态。FrameCodec 和现有 Reactor 保持原样。

M2已由UploadHandler/UploadStorage接入实际块校验、写入与持久回执；本文件描述独立codec职责，完整使用见M2_USAGE.md。

## 字节格式

所有偏移均相对消息体；外层仍使用 32 字节 Frame 头，type=5。

| 字段 | 偏移 | 长度 | 格式 |
|---|---:|---:|---|
| session_id | 0 | 16 | 原始字节 |
| offset | 16 | 8 | uint64 大端 |
| data_length | 24 | 4 | uint32 大端 |
| chunk_sha256 | 28 | 32 | 原始摘要，非 hex 文本 |
| data | 60 | data_length | 二进制原文 |

消息体总长不超过 1 MiB，因此 `MaxChunkDataSize = 1048516`；1 MiB 原文无法再加业务头放入一帧。头部不使用结构体内存布局，64 位字段不经过浮点数。

## 接口和检查

- `DataChunk` 拥有 16 字节会话 ID、uint64 offset、32 字节摘要及 `std::string data`。数据长度由 data.size 推导，避免对象内部存在两个不一致的长度。
- `encodeDataChunk` 返回消息体；分配前拒绝空数据、超限数据和 offset+length 溢出。
- `decodeDataChunk(string_view)` 接收完整消息体。先检查总长，再读取固定字段，再确认声明长度恰好等于实际数据区；拒绝截断、尾随字节、空数据、超限及 offset 溢出，然后才分配数据。失败抛 `DataChunkError`，不返回部分结果。
- `validateDataChunkRange(chunk, fileSize, chunkSize)` 接收未来可信会话里的文件大小与分块大小：chunkSize 必须为 1..MaxChunkDataSize，offset 必须小于文件大小且对齐，数据长必须等于 min(chunkSize, fileSize-offset)。只允许末块较短；空文件不发送 DATA_CHUNK。函数也重复检查结构上限与溢出，可用于尚未序列化的对象。

会话 ID 和摘要按原样保留，包括全零和高位字节；本层不判断会话是否存在，不计算或声称通过 SHA-256 校验。实际服务端仍须验证连接握手/请求关联、会话归属、块摘要和重复提交。协商出的更小 payload 上限也必须由业务调用方额外落实，工具目前检查协议硬上限。编码成功不代表服务端可接收或已持久化。

FrameCodec 负责 TCP 半包/粘包，正文完整后再调用消息体解码；本接口不会返回 NeedMore。外层识别 type=5 后，业务处理器才选择调用它。Echo 模式仍将 type=5 当作不透明测试字节，握手模式仍不支持上传。

## 所有权、线程与复杂度

解码时数据区复制一次到拥有的 string，避免视图悬空；返回对象可以移动到未来 DiskExecutor 工作函数。原输入只需在解码调用期间有效。编码分配完整消息体并复制一次数据。固定字段为 O(1)，编解码时间 O(N)、输出/解码数据存储 O(N)，N 被协议上限约束；范围校验为 O(1)。不引入共享可变状态，各调用可独立并发使用，但调用期间不能由其他线程修改输入对象/字节。

## 验证与调试

```bash
ssh miniob-ubuntu
cd /home/ning/ChunkHarbor
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 2
ctest --test-dir build --output-on-failure
ctest --test-dir build -R '^data_chunk_test$' -V
gdb --args ./build/chunkharbor-data-chunk-test
# break chunkharbor::protocol::decodeDataChunk
# break chunkharbor::protocol::validateDataChunkRange
# run
```

Ubuntu 使用前轮已缓存的本地 OpenSSL 开发包配置，完整首次配置方法见 [SHA256.md](SHA256.md)。虽然新测试不依赖 OpenSSL，完整项目默认启用哈希工具。ASan/UBSan：

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCHUNKHARBOR_SANITIZERS=ON
cmake --build build-asan -j 2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-asan --output-on-failure
```

Windows 无需 OpenSSL 的协议测试（本机 Ninja 路径沿用已有缓存）：

```powershell
cmake -S . -B build-windows-protocol -DCHUNKHARBOR_BUILD_HASH=OFF
cmake --build build-windows-protocol -j 2
ctest --test-dir build-windows-protocol --output-on-failure
```

覆盖：固定 golden vector（包括 abc 的已知摘要字节）、所有短正文、尾随字节、0/不匹配/UINT32_MAX 声明长度、原文容量边界、二进制 NUL/高位字节、独立所有权、UINT64 边界和溢出、对齐/完整块/末块/空文件/超出文件范围、FrameCodec 每个切分点和两帧拼接。测试只在内存运行。
