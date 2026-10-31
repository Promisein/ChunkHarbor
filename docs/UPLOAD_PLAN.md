# UPLOAD_PLAN 控制消息（M2 独立编解码）

实现为纯 C++17 的 `protocol/UploadPlan.h` / `src/protocol/UploadPlan.cpp`，属于 `chunkharbor_protocol`。本轮只实现消息体与请求几何检查，不生成会话、不访问文件、不提供上传入口。外层 Frame type=4，request_id 和当前请求关联由后续业务调用方检查。

上述为独立编解码器的职责；M2当前已由UploadStorage生成新会话全量计划，CLI检查请求关联后实际上传，见M2_USAGE.md。分页/增量/重启续传仍未实现。

## 字段及限制

JSON必须恰好包含三个字段，不允许缺失、未知或重复键（包括解码转义后重复）。

```json
{"session_id":"0123456789abcdef0123456789abcdef","chunk_size":262144,"missing_ranges":[[0,3]]}
```

| 字段 | 约束 |
|---|---|
| session_id | 32个小写十六进制字符，解码为16字节会话ID |
| chunk_size | JSON整数，1..1048516，匹配DATA_CHUNK硬上限 |
| missing_ranges | 0..512个区间；每个恰好两个uint64整数，表示块索引半开区间[begin,end) |

区间必须begin<end、升序且不重叠；相邻区间允许（如[0,1)、[1,3)），解析不进行排序、合并或修复。拒绝重复区间、倒序和空区间。整数不经过double，支持完整uint64范围；拒绝负数、浮点/指数、字符串、布尔和溢出。示例[0,3)表示块0、1、2。

协议检查保留ID所有原始字节（包含全零）；ID是否由服务端安全随机生成、是否存在、是否属于此客户端必须由业务层验证。对象不能赋值一个不合法长度的ID，因为C++字段固定为 `array<uint8_t,16>`，编码始终输出规范小写hex。

消息体上限64KiB，解析前拒绝超限、原始NUL和BOM；拒绝注释、尾逗号、尾随非空白、多JSON文本、非法UTF-8、嵌套对象或多余数组层级。解析回调在构建多余DOM前拒绝未知/重复键、过深数组及第513个区间。正常JSON前后空白允许。编码为确定性紧凑JSON，键按字典序。无效输入抛 `UploadPlanError`，不对外暴露JSON库类型。

## 接口和几何校验

- `encodeUploadPlan` / `decodeUploadPlan`：只处理完整消息体；输入不完整即报错。TCP半包/粘包仍交给FrameCodec。
- `UploadPlan` 拥有ID、块大小及 `vector<ChunkRange>`，返回值不引用原始字节；可移动到后续任务编排层。
- `validateUploadPlanForFile(plan,fileSize,requestedChunkSize)`：重复结构验证，要求返回块大小与可信请求相同；计算块总数 `fileSize/chunkSize + (fileSize%chunkSize != 0)`，避免 `fileSize+chunkSize-1` 溢出。每个区间end不得超过该块数。空文件只接受空计划；最后不足一块仍计为一块。

空计划代表“本条计划没有请求数据块”，不能据此判定服务器已存在文件或已完成持久化。该函数允许稀疏子集，M2首次上传业务之后必须另行保证全量接收计划正确；本轮不实现断点续传或增量对比。不信任未经关联检查的服务器消息，也不允许以客户端提供的会话ID任意覆盖服务端会话。

512个区间是本轮M2小型计划的明确资源上限，并非最大512个块。一个区间可表达大量连续块，代码不枚举其中的块，也不累计区间长度，因此uint64极值不会诱发按块数分配。无法装入单条计划的任务应被后续业务拒绝或在M4使用独立分页协议；本轮不会截断区间或提前支持分页。

## C++与资源

使用固定宽度整数、拥有数据的值对象及RAII容器；无共享可变状态，同一输入在调用期间不可被其他线程修改。协议库不依赖Qt、OpenSSL或POSIX。编码前检查区间数和结构，再构建有界JSON；解码先限制字节数/嵌套/区间数，再保留解码区间。对N字节JSON、R个区间，编解码时间/额外空间为O(N+R)，几何验证O(R)、额外空间O(1)，与区间覆盖的块数无关。OOM不转换成协议错误。调用方仍需协商上限、内存队列上限、配额、会话生命周期和持久ACK。

## 验证与调试

Ubuntu沿用正式目录缓存（OpenSSL配置见SHA256.md）：

```bash
ssh miniob-ubuntu
cd /home/ning/ChunkHarbor
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 2
ctest --test-dir build --output-on-failure
ctest --test-dir build -R '^upload_plan_test$' -V
gdb --args ./build/chunkharbor-upload-plan-test
# break chunkharbor::protocol::decodeUploadPlan
# break chunkharbor::protocol::validateUploadPlanForFile
# run
```

ASan/UBSan：

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCHUNKHARBOR_SANITIZERS=ON
cmake --build build-asan -j 2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-asan --output-on-failure
```

Windows使用已有Ninja/MinGW缓存，无需OpenSSL：

```powershell
cmake -S . -B build-windows-protocol -DCHUNKHARBOR_BUILD_HASH=OFF
cmake --build build-windows-protocol -j 2
ctest --test-dir build-windows-protocol --output-on-failure
```

测试只在内存运行：固定JSON向量、ID字节对应/格式、字段数量及类型、整数精度/溢出、空/相邻/稀疏区间、重叠/倒序/重复/空区间、512/513区间、空文件/末块/整块及UINT64_MAX附近的安全块数计算、64KiB边界、原始NUL/非法UTF-8/恶意嵌套及FrameCodec每个切分点/粘包组合。
