# M3 SQLite 会话与发布协调

此文档描述当前实现。服务端仅供可信 loopback/SSH 使用；没有认证、TLS 或多用户会话授权。

## 持久状态与线程所有权

备份根目录保持 M2 的排他锁。`metadata.sqlite3` 位于该私有根目录，采用 WAL、`synchronous=FULL`、外键及 v1 事务迁移；未知版本拒绝启动。`transfers` 保存服务端生成的随机 session ID、逻辑路径、文件大小/摘要、块大小、mtime、暂存路径和状态；`transfer_chunks` 保存持久确认的块索引和摘要。同一路径只允许一个活跃会话。SQL 参数绑定，不拼接用户输入。网络事件循环不执行 SQLite 事务，上传和查询都经有界磁盘工作池；共享数据库连接受互斥量保护。

Windows CLI 的 `--state` 文件由 `QSaveFile` 原子写入，保存 session ID、源文件绝对路径、逻辑路径、大小、块大小和整文件 SHA-256。恢复前重新哈希源文件并核对这些字段；服务端数据库是实际块进度的最终依据。`status --state` 查询保存的会话，`status --session-id` 可按历史 ID 查询活跃或已提交状态。本地状态文件和 session ID 是会话持有凭据，应保存在私人目录。

## ACK 与查询

新会话：创建并同步私有暂存目录、数据文件和元数据，提交 SQLite `CREATED`，然后返回 `UPLOAD_PLAN`。若提交失败，不返回计划；无数据库记录的残留暂存目录在启动时移入 `.quarantine`。

新块：校验范围和 SHA-256 → 完整 `pwrite` → `fdatasync(data)` → 同步 M2 回执日志 → SQLite 事务插入块及更新 `RECEIVING` → 事务成功后才发送 `CHUNK_ACK`。重启后回执日志不作为进度真相；每个数据库确认块还须重新读取并校验摘要。核验失败的块从数据库确认集移除，客户端查询时会看到缺失位。

`QUERY_SESSION`(7) 携带严格的 32 字符小写十六进制 session ID。活跃会话返回 `SESSION_STATUS`(8)：`session_id`、`chunk_size`、`block_count`、`missing_bitmap`。位图按块索引排列，字节内低位在前，1 表示缺失；最多 65536 块、8192 字节，避免区间高度交错时超出控制帧上限。已提交会话返回 `COMMIT_RESULT`(10)，无效或不可恢复会话返回 `SESSION_NOT_FOUND`。客户端只发送位图标记缺失的块。重复块需匹配已确认摘要；重复提交可通过查询或直接 `COMMIT_UPLOAD` 得到已提交结果。

## 发布与恢复

全部块齐全后重新计算整文件 SHA-256；匹配后同步暂存文件，数据库从 `CREATED/RECEIVING` 经 `VERIFYING` 进入 `COMMITTING`。执行 `renameat2(RENAME_NOREPLACE)`、同步目标父目录，再把数据库标为 `COMMITTED`，最后返回提交结果。数据库与文件系统不构成原子事务。

启动时先核验、再开始监听：

| 状态或不一致 | 处理 |
|---|---|
| CREATED/RECEIVING 暂存存在 | 核对元数据与数据库，逐块重算 SHA-256；损坏块重新列为缺失 |
| VERIFYING 暂存存在 | 重置为 RECEIVING，允许再次执行整文件核验 |
| COMMITTING 暂存存在、目标不存在 | 先核验已确认块；若有损坏块，移除其确认并退回 RECEIVING；若完整，重复提交时重新核验整文件并执行不覆盖发布 |
| COMMITTING 目标已存在 | 校验目标大小及 SHA-256，同步父目录后补写 COMMITTED |
| COMMITTED | 校验目标大小及 SHA-256；目标缺失或不符则拒绝启动 |
| 有暂存目录但无数据库记录 | 移至 `.quarantine`，不自动当作有效会话 |
| 有活跃数据库记录但暂存目录缺失 | 标为 FAILED，查询不提供错误进度 |
| FAILED 暂存目录仍存在 | 移至 `.quarantine` 供人工检查 |
| CREATED/RECEIVING 超过30天未更新 | 启动时标为 EXPIRED，清理仅该会话受限目录中的暂存文件 |

不识别的目录项、元数据冲突、发布文件哈希不符等情况保守拒绝启动，避免覆盖或误报成功。旧 M2 没有数据库记录的未完成会话会隔离，不能自动升级续传。当前无正式文件累计容量配额、历史版本或硬件掉电实测；详见 `M3_VALIDATION.md`。
