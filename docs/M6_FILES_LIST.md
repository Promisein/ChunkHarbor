# M6 受限分页文件列表

本轮仅实现协议与服务端只读查询，不实现下载、GUI列表或历史快照。

## 契约

- HELLO 双方协商 `file_list_v1`；仅 upload 服务模式提供该能力，与 upload/delta 能力独立。
- 类型11 LIST_FILES：`{"after_path":"","limit":32}`。字段必填，limit为1至64的整数；空游标从头开始，非空游标必须是合法相对UTF-8逻辑路径。
- 类型12 FILES_RESULT：`{"files":[{"path":"a.txt","version_id":"32位小写十六进制","size":123,"sha256":"64位小写十六进制"}],"next_after_path":""}`。
- 按路径UTF-8字节序升序，每条路径只返回当前已提交版本。路径、哈希遵循既有上传校验；size为无损uint64 JSON整数，客户端不得经double读取。
- 响应最多64条、JSON负载最多65536字节，长路径导致提前截页。非末页游标等于本页最后路径，末页为空；不能出现空的非末页。
- 使用keyset查询（path > after_path），不使用OFFSET或全量目录扫描。跨页是实时视图，不是快照：并发插入到游标之前的路径需重新从头刷新才能看见。
- 当前版本来自current_files；尚未映射的旧upload COMMITTED记录也可见，版本标识沿用其session_id，与既有legacy迁移约定一致。暂存、失败、preview和历史版本不展示。
- 查询不读取文件正文、不计算哈希、不触发legacy迁移；SQLite已有提交元数据不代表对外部篡改进行了本次重新核验。
- 非法字段返回MALFORMED_MESSAGE；未协商返回UNSUPPORTED_OPERATION；缺少HELLO返回HANDSHAKE_REQUIRED；忙时沿用BUSY。列表不绑定或更改上传会话。
- 当前仍仅可信loopback/SSH，无身份认证或多用户隔离，不宣称“授权文件列表”。

## 实施与验收计划

1. 先添加协议测试及真实TCP集成测试，确认旧服务端不支持列表的失败证据。
2. 新增纯C++17 FilesList编解码；严格校验JSON字段、顺序、游标、路径、整数和资源上限。
3. SqliteStore新增受限keyset查询，为COMMITTED旧上传增加部分路径索引（兼容schema v2，无版本升级）；UploadStorage持有数据库锁组装有界响应。
4. UploadHandler协商能力并投递到现有DiskExecutor；仅loop读写连接，weak_ptr完成回调保留既有生命周期和BUSY行为。
5. Windows共享协议/GUI回归；SSH Ubuntu独立源码和私有测试根完成配置、构建、全量CTest及相关集成测试；更新PROGRESS。

涉及：FilesList.h/.cpp、SqliteStore、UploadStorage、UploadHandler、CMake及专项单测/集成测试。不改GUI、不扫描真实用户数据、不自动提交或推送。
