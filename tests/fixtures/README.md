# R2.2 旧库备份夹具

`r22-v2-backup.tar.gz` 是 Ubuntu 上提交 `f00f601` 的真实服务端在专用根生成的 schema v2 备份。归档保存了服务端停止时的数据库、版本对象、当前文件和暂存数据。

- 用旧 CLI 的 `backup` 对 `fixture/file.txt` 先后上传内容 `R2.2 fixture A`、`R2.2 fixture B`、`R2.2 fixture C`。
- 用旧 CLI 的 `upload` 提交 `fixture/legacy.txt`，内容为 `legacy fixture`。
- 使用旧协议建立 `fixture/inflight.txt`，内容 `ABCDEFGH`，块大小 4，发送并确认首块 `ABCD` 后断开。

SHA-256 为 `66f39a57dc5d0d355de77dbc29f2717e82980384a09782ceda6ae0d31f205151`。测试始终解压到专用临时目录。会话租期已经接近或超过期限时，只在副本上调整 `delta_sessions.created`，以便长期复用同一夹具来检验续传迁移。

## R2.4 schema v3 夹具

`r24-v3-backup.tar.gz` 由 R2.3 验证过的 Ubuntu 服务端在 v2 归档副本上实际启动、迁移至 v3、正常停止后生成。含 A/B/C、旧上传、四条历史登记和原有续传检查点。测试在其副本上验证 v3→v4 迁移和快照事务。

SHA-256 为 `75985bb33fca0fa915c90cf90d928844c4e12455694c5d5290ccb04786e24197`。归档本身在验证中保持原样。
