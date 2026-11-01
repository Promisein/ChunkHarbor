# M4 首个子任务：MANIFEST_PAGE 协议设计草案

日期：2026-11-01。状态：后续用户已授权实现；任务1及完整M4现已完成，本文保留首个模块的设计上下文。当前结果见PROGRESS/M4_VALIDATION。

## 目标与范围

用户确认 M3 人工验收通过并要求开始 M4。本轮以现有工程为基础，只设计一个可独立测试的子任务：type 18 `MANIFEST_PAGE` 的纯 C++17 编解码和单页/文件上下文校验。采用固定索引 SHA-256 比较，不是 rsync rolling checksum。

本草案中的字段、512项固定分页及接口细节是本轮建议，尚未由用户确认。后续实现不修改上传业务、数据库、CLI 或 GUI，不宣称完成增量传输。

## 已核对的现状

- `FrameCodec` 已识别 type 1–23，使用32字节大端帧头和64KiB控制负载上限；识别type18不等于业务支持。
- `BeginUpload` 是严格五字段对象，服务端拒绝已存在目标，不能直接拿旧流程覆盖同路径文件。
- 旧 `UploadPlan` 最多512个区间；高度交错差异可能超限，M4必须另做分页计划。
- M3单文件上限64MiB、65536块；共享protocol不依赖Qt/POSIX/OpenSSL。SHA-256在业务层计算，协议层只传固定32字节摘要。
- 本子任务保留M3限制。M3暂存清理中断后的启动问题保留在PROGRESS风险记录，按用户要求不作为启动本协议子任务的前置条件。

## 线格式

沿用wire version 1的既有帧格式，使用预留type18；不改变任何已实现消息的字段、编号、字节序或现有golden vector。未来接入服务端时必须单独协商增量能力；本子任务不宣告新能力，也不让服务端接受type18。

payload为UTF-8 JSON对象，严格且恰好包含以下四字段：

| 字段 | 类型 | 约束 |
| --- | --- | --- |
| `session_id` | string | 服务端生成的16字节会话ID，以32位小写十六进制表示 |
| `page_index` | uint32整数 | 从0开始，范围0..127 |
| `first_block_index` | uint32整数 | 必须等于 `page_index * 512` |
| `hashes` | string数组 | 1..512项，每项64位小写十六进制SHA-256 |

`hashes[j]` 对应当前源文件块 `first_block_index + j`，并不是可任意排序的哈希集合。连续分页隐含块索引，避免每项重复携带索引；非末页必须512项，末页长度由文件上下文验证。

合法的单块示例（摘要为ASCII `abc` 的SHA-256）：

```json
{"session_id":"0123456789abcdef0123456789abcdef","page_index":0,"first_block_index":0,"hashes":["ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"]}
```

编码输出为无空白紧凑JSON，按现有nlohmann/json默认对象键顺序：`first_block_index`、`hashes`、`page_index`、`session_id`；解码接受任意键顺序和合法外围空白。测试应固定精确payload及完整type18帧的golden vector。

## 两层校验

结构编解码必须拒绝空正文、超过65536字节、BOM、原始NUL、非法UTF-8、尾随非空白、重复/未知/缺失键、错误类型、负数、浮点数、整数越界、错误ID和摘要格式。除顶层对象及`hashes`字符串数组外不接受嵌套容器；不得先经double转换整数。JSON语法/类型异常统一转为`ManifestPageError`，不吞掉内存分配等系统异常。

`first_block_index`与页号须匹配，`first_block_index + hashes.size()`不得超过65536；先检查边界再缩窄或分配。数组解析回调须在第513项进入结果向量之前拒绝；解码前先检查整个payload上限。编码前也验证对象及最终序列化字节数。

文件上下文校验接收可信的会话ID、文件大小、块大小和下一期待页号：

- chunk size范围沿用`MaxChunkDataSize`，不能用1MiB替代扣除DATA_CHUNK业务头后的真实上限。
- block count使用 `size / chunkSize + (size % chunkSize != 0)`，避免加法向上取整溢出；拒绝文件超过64MiB或块数超过65536。
- 页ID必须与可信会话相同，页号必须与下一期待页号相同，不能混入其他源快照的页面。
- 起始块必须小于总块数，页内项数必须恰好为 `min(512, blockCount - firstBlockIndex)`。
- 空文件不发送任何MANIFEST_PAGE；不能用空hashes数组表达完成。

单页解码不判断整份manifest是否完整。重复/乱序由调用方提供的期待页号拒绝；该纯函数不持有连接、累计页或修改服务端状态。语法合法也不证明摘要匹配源文件内容或基线版本。

## 拟实现接口及文件

新增 `include/chunkharbor/protocol/ManifestPage.h`、`src/protocol/ManifestPage.cpp`、`tests/unit/manifest_page_test.cpp`；修改 `CMakeLists.txt` 注册到既有protocol target及CTest，更新协议/进度文档。保持旧解析器原样，不为本任务抽取全局JSON框架。

```cpp
namespace chunkharbor::protocol {
constexpr std::size_t MaxManifestPageHashes = 512;
class ManifestPageError : public std::runtime_error { /* inherit constructor */ };
struct ManifestPage {
    std::array<std::uint8_t, 16> sessionId{};
    std::uint32_t pageIndex = 0;
    std::uint32_t firstBlockIndex = 0;
    std::vector<std::array<std::uint8_t, 32>> hashes;
};
std::string encodeManifestPage(const ManifestPage& value);
ManifestPage decodeManifestPage(std::string_view payload);
void validateManifestPageForFile(
    const ManifestPage& value,
    const std::array<std::uint8_t, 16>& expectedSessionId,
    std::uint64_t fileSize, std::uint32_t chunkSize,
    std::uint32_t expectedPageIndex);
}
```

接口/常量标识符及错误分类按以上约定实现；私有辅助函数命名、测试辅助代码安排留给实现者。头文件应直接包含所需标准库头，不依赖偶然的传递包含。

## 线程、资源与失败路径

协议函数同步执行且无共享可变状态，输入string_view仅在调用期间借用；返回对象拥有全部摘要，不引用输入缓冲。单页摘要最多16KiB；禁止新增存储全部页面的容器。JSON文本及DOM仍有额外但有界开销，不宣称总内存只有16KiB。

无磁盘、socket、锁、线程、数据库或QObject资源。本轮未来实现只验证/序列化摘要，实际文件扫描与哈希仍必须在客户端工作线程，服务端比较/持久化仍必须在有界磁盘工作池。错误通过异常返回给上层，本任务不发送网络ERROR，不将单页成功称为持久ACK。

## 与后续M4的衔接边界

后续仍需独立设计/实现以下内容，不能把本模块测试通过算作它们已完成：

1. 基线选择与增量会话建立：明确`base_version`、不可变版本内容和版本前置条件；旧BEGIN_UPLOAD保留严格格式，是否新增消息或通过明确版本/能力扩展须单独决策。当前服务端没有建立同路径增量会话的入口。
2. MANIFEST_DONE：会话身份、实际页数与块数、完整覆盖验证；空文件零页。明确每页请求的响应/背压机制及清单重试持久性后，再接网络状态机；不得靠超时推断清单完成。
3. UPLOAD_PLAN_PAGE/DONE：按有界页返回半开缺失区间，页序号/身份/请求关联完整；DONE前禁止开始数据上传。不能将分页结果塞回受512区间限制的旧UploadPlan。
4. 从固定基线复制相同索引、相同长度且哈希匹配的块到新暂存文件；复制块也须校验和持久化才能计为已确认。提交前校验整文件及基线前置条件，不能直接覆盖旧正式文件。
5. 目录批任务和只读预览复用相同基线、差异计算及执行前复核；后续验证无变化0 DATA_CHUNK、单块变化只传1块、增长/缩短/头部插入和冲突。

## 首个实现子任务的验收清单

- 固定JSON/type18帧golden vector；完整帧、逐字节拆包、与后续帧粘包的组合测试。
- 1项、512项、最后一页、最大块索引65535、128页边界；0项/513项、页号128、起始错位和越界拒绝。
- 会话ID/摘要hex与字节数组往返；大写、长度错误和非法字符拒绝。
- JSON重复/未知/缺失键、嵌套、负数/小数/超uint32/超uint64、NUL尾缀、UTF-8/BOM、65536字节边界拒绝或接受符合条件的外围空白。
- 文件0/1/511/512/513/65536块；非整末块、错误会话、重复/跳页、短非末页、空文件页面、零/超限chunk size、超文件/块数限额、uint64最大值拒绝且无溢出。
- 先用新增测试验证空实现失败，再接入代码；Windows纯协议和Ubuntu完整回归，实际命令/结果写PROGRESS；环境不足则明确未执行。不得用本轮编解码单测声称增量业务可用。

## 本轮交付状态

本轮只新增此草案并更新进度；没有修改CMake、源码或测试，没有提交/推送。设计经用户确认后，下一轮仅实现上述MANIFEST_PAGE模块和对应测试。
