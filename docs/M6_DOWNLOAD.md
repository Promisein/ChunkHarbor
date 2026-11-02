# M6 Linux 单文件下载设计与实施

本轮仅实现下载协议、服务端会话及Linux CLI恢复，不接GUI、不实现下载续传/覆盖/历史版本选择。

## 协议与资源

- HELLO独立协商`download_v1`，仅服务端upload模式提供。
- 13 BEGIN_DOWNLOAD：`{"path":"docs/a","version_id":null}`，可填列表返回的32位小写十六进制版本作为当前版本前置条件。不存在返回FILE_NOT_FOUND，当前版本不符返回VERSION_CONFLICT。
- 新增32 DOWNLOAD_READY：`{"path":"docs/a","version_id":"...","size":123,"sha256":"..."}`。大小最大64MiB，整数无损。
- 新增33 GET_DOWNLOAD_CHUNK：`{"version_id":"...","offset":0,"length":262144}`。length为1至256KiB，范围必须位于文件内。
- 14 DOWNLOAD_CHUNK复用DATA_CHUNK的显式大端格式，前16字节为version_id，其余为offset、长度、块SHA-256及二进制内容；最大数据256KiB。空文件仅READY，无数据块。
- 每连接最多一个下载会话，与上传会话互斥；列表可共用连接。固定FD保证并发新版本发布后仍读取原版本。每次请求仅产生一个块，以拉取形成自然背压。
- 打开时在工作池校验整文件SHA-256；块读取/哈希也在有界工作池。数据库锁仅用于选择元数据，不包围正文读取。断连后RAII释放会话FD，在途任务仅持有自己的shared_ptr；完成通过weak_ptr回所属loop，不写已销毁连接。
- 新下载只选择当前已提交版本。兼容旧upload尚未迁移记录：查询其COMMITTED元数据并安全打开files下原文件；不进行全库legacy迁移。禁止打开暂存文件、符号链接、FIFO或越界路径。

## CLI与发布

`chunkharbor-cli download --path LOGICAL --output LOCAL [--version-id ID] [--host 127.0.0.1] [--port 8787]`

目标父目录须已存在，逐组件openat/O_NOFOLLOW固定目录FD；拒绝..、符号链接及已有最终目标。服务端逻辑路径不用于拼接本地目标。目标目录须由调用者控制，不对抗同UID恶意进程。

临时文件在同目录以随机名O_EXCL/0600创建，RAII正常异常清理。CLI核对request_id、类型、路径/版本、块偏移/长度/哈希及整文件哈希，fsync临时文件后renameat2(RENAME_NOREPLACE)，再fsync父目录。发布竞争不覆盖既有文件。目录fsync若失败，已发布文件可能存在，CLI报失败而不删除已发布结果；强制终止可能留下隐藏临时文件，不声称下载断点续传。

## 最小实施计划

1. 先添加真实TCP/CLI恢复测试，确认基线缺少download_v1；增加协议边界单测和恶意服务端测试。
2. 实现纯C++17 Download编解码，增加32/33帧类型，不修改原消息语义。
3. 实现UploadStorage下载会话及UploadHandler工作池投递/互斥/连接生命周期。
4. 新增Linux DownloadClient及安全临时发布，其他客户端不接入口。
5. Windows共享协议/GUI回归；SSH Ubuntu隔离源码、专用数据根，完整构建/CTest及下载集成；记录真实结果和未验证项。

修改集中在Download.h/.cpp、UploadStorage、SqliteStore、UploadHandler、Linux DownloadClient/main、FrameCodec、CMake、测试与文档；保留上一轮未提交的分页实现，不自动提交或推送。
