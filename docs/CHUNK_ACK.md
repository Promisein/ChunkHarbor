# CHUNK_ACK：严格消息体与持久确认契约

`protocol/ChunkAck.h`、`src/protocol/ChunkAck.cpp` 是独立纯C++17编解码。M2完整上传已接入UploadStorage和运行时ACK，使用同步数据加同步追加回执作为最小持久元数据；不使用SQLite、不支持重启续传，使用和失败窗口见M2_USAGE.md。

## 五字段消息体

指南规定了确认语义但未固定ACK字段，本轮确定以下字段。Frame type=6，外层request_id必须由后续业务层与对应DATA_CHUNK关联。

```json
{"session_id":"0123456789abcdef0123456789abcdef","offset":0,"data_length":3,"chunk_sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","durable":true}
```

| 字段 | 规则 |
|---|---|
| session_id | 恰好32个小写hex字符，对象内为16字节 |
| offset | JSON整数，完整uint64范围，并检查offset+data_length不溢出 |
| data_length | JSON整数，1..1048516，与DATA_CHUNK数据区一致 |
| chunk_sha256 | 恰好64个小写hex字符，对象内为32字节 |
| durable | 必须是JSON布尔true；false、数字1、字符串true及缺失均拒绝 |

字段必须恰好五个，拒绝重复（包括转义后重复的键）、未知、缺失、错误类型、浮点/指数或整数溢出。控制正文上限64KiB，解析前检查长度、原始NUL和BOM，严格拒绝非法UTF-8、注释、尾逗号、多个JSON值、尾随垃圾、嵌套对象/数组。允许正常JSON外层空白。编码输出确定性紧凑JSON，键按字典序。无效输入抛 `ChunkAckError`。

`ChunkAck` 只表示一种确认语义，所以对象没有可设为false的durable开关；编码始终输出true，解码只接受true。未持久化进度不得复用该类型或发送同名弱ACK。全零ID/摘要等按原始字节保存；能编解码不证明ID有效或摘要正确。

## 持久确认的发送条件

M2服务端在返回并发送CHUNK_ACK前完成以下步骤：

1. 验证握手、会话归属、请求关联、offset对齐、文件范围、长度与重传一致性，实际计算并比对块SHA-256。
2. 在安全创建的会话暂存文件里完成全部字节写入，正确处理部分写和错误；文件名、目录项及会话关联须已具备约定的可恢复持久性。
3. 完成约定的 `fdatasync` / `fsync` 等数据持久化步骤；如有新目录项，按存储设计完成相应目录持久化。仅写入页缓存不算完成。
4. 提交块确认元数据。M2先持久化meta和具名会话数据/目录，再使用 `pwrite → fdatasync(data) → 完整追加ACK回执 → fdatasync(receipts.log) → ACK`；只在成功后设置内存accepted位。数据/回执写入或同步错误会poison会话，不返回ACK。指南的SQLite事务策略由M3实现，不能把本轮追加日志称作SQLite事务或恢复协议。
5. 工作完成后经DiskExecutor完成通道回到所属EventLoop，再检查连接/会话仍有效，然后编码发送。队列满、工作失败或持久步骤失败时不得发送成功ACK。

编解码器只能表达声明；持久前提由UploadStorage业务执行，并以实际同步错误注入测试验证不返回确认。所有阻塞操作在worker。未完成会话断连后保留数据和回执、继续占配额；启动只校验并计入限制，不重建可续传会话。M3的SQLite、崩溃协调和查询尚未完成；断电恢复没有验收证据。

ACK不表示整个文件已校验、原子发布或提交成功；最终成功另需整文件校验和提交/发布结果。文件系统与数据库不是一个原子事务，仍有崩溃窗口，必须通过恢复协议协调。

ACK可能在持久完成后丢失；同连接/会话中相同块重复请求返回同义ACK，不同内容拒绝覆盖。CLI逐请求等待并关联ACK，不会接受无关重复响应增加confirmed；跨重连/进程恢复仍未实现，不能因此宣称断点续传。

## API与匹配检查

`encodeChunkAck`、`decodeChunkAck`只处理完整消息体，半包/粘包仍由FrameCodec处理。`validateChunkAckForChunk(ack,pending)`重复结构校验，再逐一比对会话ID、offset、长度和待确认DATA_CHUNK声明的摘要。它不计算实际数据哈希、不检查Frame type/request_id、不验证发送方身份或磁盘状态；调用方须独立完成这些检查。空文件没有数据块，因此不能产生零长度CHUNK_ACK。

值对象使用固定数组、固定宽度整数，局部DOM/字符串由RAII释放；不保存输入视图，无共享可变状态。调用期间输入不得被其他线程修改。编解码时间/内存O(N)，N受64KiB限制；块匹配只比较固定元数据和string长度，O(1)，不复制块内容。协议库无Qt、OpenSSL或POSIX依赖；OOM不转换成协议字段错误。

## 实际验证与调试命令

Ubuntu正式目录沿用已有配置（OpenSSL开发包说明见SHA256.md）：

```bash
ssh miniob-ubuntu
cd /home/ning/ChunkHarbor
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 2
ctest --test-dir build --output-on-failure
ctest --test-dir build -R '^chunk_ack_test$' -V
gdb --args ./build/chunkharbor-chunk-ack-test
# break chunkharbor::protocol::decodeChunkAck
# break chunkharbor::protocol::validateChunkAckForChunk
# run
```

ASan/UBSan：

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCHUNKHARBOR_SANITIZERS=ON
cmake --build build-asan -j 2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-asan --output-on-failure
```

Windows已有Ninja/MinGW缓存：

```powershell
cmake -S . -B build-windows-protocol -DCHUNKHARBOR_BUILD_HASH=OFF
cmake --build build-windows-protocol -j 2
ctest --test-dir build-windows-protocol --output-on-failure
```

协议单测覆盖固定JSON、durable类型/值/缺失、字段、hex/UTF-8/NUL、整数精度/溢出和帧边界；另有upload_storage实际文件与写入/同步故障注入、upload_integration真实ACK闭环、cli_upload_peer_test伪造ACK拒绝。没有真实断电或kill -9自动恢复验证。
