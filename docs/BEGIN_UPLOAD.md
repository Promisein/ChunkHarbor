# BEGIN_UPLOAD 控制消息（M2 独立编解码）

公共 API：`protocol/BeginUpload.h`，实现在 `src/protocol/BeginUpload.cpp`，属于纯 C++17 的 `chunkharbor_protocol`。只处理完整 JSON 消息体；调用方负责外层 Frame type=3、request_id、握手、授权、会话状态及协商上限。现有 Echo/握手模式尚不支持上传。

M2当前已有独立 `--upload` 业务模式，接入此消息分配安全会话，详见M2_USAGE.md；本文件的编解码器本身不创建文件。

## 严格字段契约

必须恰好包含以下五个字段，不接受缺少、未知或重复字段（包括经 JSON 转义后重复的键）。

| JSON 字段 | C++ 字段/类型 | 约束 |
|---|---|---|
| path | path / string | 有效 UTF-8 的相对逻辑路径，统一 `/` 分隔 |
| size | size / uint64_t | 0..UINT64_MAX，允许空文件 |
| mtime_ns | mtimeNs / int64_t | INT64_MIN..INT64_MAX，允许 epoch 前负时间 |
| chunk_size | chunkSize / uint32_t | 1..1048516，与 DATA_CHUNK 1MiB 总 payload 减60字节头一致；默认256KiB |
| sha256 | sha256 / string | 恰好64个小写十六进制字符 |

数字必须为 JSON 整数字面值；拒绝小数、指数、布尔、字符串以及超出目标整数范围的值。解析和序列化均直接使用整数，不经过 double / Qt JSON 浮点值。完整 uint64 大小是协议表达范围，后续存储层仍须按 off_t、配额和实现能力收窄，不能将其全部视为可落盘承诺。

可运行字段样例（abc 的真实摘要）：

```json
{"path":"a.bin","size":3,"mtime_ns":1730000000000000001,"chunk_size":262144,"sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}
```

本轮确定的逻辑路径限制：总长1..4096 UTF-8字节，每个分量1..255字节；拒绝 `.`、`..`、空分量、前/后/重复斜杠、反斜杠、冒号、NUL及ASCII控制字符/DEL。允许中文和emoji。只检查原始逻辑名，不 URL 解码、不 Unicode 归一化、不静默替换分隔符。Windows 本地盘符路径必须先由客户端生成独立相对逻辑路径再编码。Windows 保留设备名、尾随点/空格等恢复兼容策略尚未实现，不代表所有合法逻辑名均能原样恢复到 Windows。

消息体为1..65536字节；在JSON解析前检查输入总长并拒绝原始NUL，另拒绝BOM、非法UTF-8、注释、尾随逗号、多个JSON文本、嵌套对象和数组。正常外层空白允许。原始NUL必须显式拒绝：当前JSON库将其当作输入结束，否则会忽略其后的字节。解析回调在构建嵌套DOM前拒绝嵌套输入，并检测解码后的重复键。编码采用确定性紧凑JSON（键按字典序），也校验字段和输出总长；非法输入抛 `BeginUploadError`，不会泄露JSON库类型到公共接口。

## 资源、边界与职责

`BeginUpload` 是拥有字符串的值对象，可复制/移动；解码不保留输入视图。调用期间输入必须有效且不能被其他线程修改，各调用无共享可变状态。JSON DOM 和局部字符串由 RAII 管理。N 字节消息的解析/编码时间与额外空间均为 O(N)，输入硬上限64KiB、输出字段另有上界。失败时不返回部分结果；进程内存耗尽异常不转换为业务错误。

摘要字段只检查形状，不读取文件或验证内容；空文件可以声明大小0，但是否匹配空文件摘要要由后续实际内容校验决定。mtime 是元数据，不以它单独判断源文件是否未变。协议解析成功不代表上传会话已经创建，更不代表数据已持久化。

路径检查只是词法规则，无法防止符号链接和目录竞态；后续存储层必须使用受限目录FD和安全打开流程。业务层仍需配额、目标冲突控制、会话归属、重试幂等及持久ACK。解析输出不应直接拼接到服务器根目录进行文件操作。本轮不修改Reactor、握手解析器或磁盘执行器。

## 构建、验证与调试

Ubuntu（沿用正式目录前轮缓存配置；首次OpenSSL依赖配置见 [SHA256.md](SHA256.md)）：

```bash
ssh miniob-ubuntu
cd /home/ning/ChunkHarbor
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 2
ctest --test-dir build --output-on-failure
ctest --test-dir build -R '^begin_upload_test$' -V
gdb --args ./build/chunkharbor-begin-upload-test
# break chunkharbor::protocol::decodeBeginUpload
# run
```

ASan/UBSan：

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCHUNKHARBOR_SANITIZERS=ON
cmake --build build-asan -j 2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-asan --output-on-failure
```

Windows已有Ninja/MinGW构建目录，无需OpenSSL：

```powershell
cmake -S . -B build-windows-protocol -DCHUNKHARBOR_BUILD_HASH=OFF
cmake --build build-windows-protocol -j 2
ctest --test-dir build-windows-protocol --output-on-failure
```

测试只在内存运行，覆盖固定JSON向量、五个必需字段逐一缺失、未知/重复/转义重复键、错误类型、整数溢出与大整数精度、块大小两端、真实摘要格式、中文/emoji、非法UTF-8、路径穿越/控制字节/路径和分量上限、恶意嵌套、JSON及64KiB边界、FrameCodec半包与粘包组合。不进行上传或磁盘写入。
