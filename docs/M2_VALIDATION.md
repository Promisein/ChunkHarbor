# M2 验收证据（2026-10-31）

范围：指南M2的Linux首次完整文件上传必需任务，保留M1自主Reactor；可选Qt原型另列，不将其视为GUI备份功能。正常运行命令见M2_USAGE.md。

## 实际运行

| 平台/配置 | 命令/目标 | 结果 |
|---|---|---|
| Ubuntu正式目录Debug | cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug；cmake --build build -j 2；ctest --test-dir build --output-on-failure | 26/26通过 |
| Ubuntu ASan/UBSan | build-asan，SANITIZERS=ON；ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-asan --output-on-failure | 26/26通过 |
| Ubuntu Release | cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release；构建及CTest | 26/26通过 |
| Windows MinGW纯协议 | build-windows-protocol，HASH=OFF、GUI/server/CLI=OFF；构建及CTest | 7/7通过 |
| Windows MinGW+OpenSSL工具 | build-windows-hash；构建及CTest | 8/8通过 |
| Windows Qt6.8.3匹配Kit | build-windows-gui，GUI=ON、HASH/server/CLI=OFF；构建及CTest | 8/8通过（含7种Qt网络场景） |

Ubuntu使用已有OpenSSL开发头缓存，第一次Release配置显式指定OPENSSL_INCLUDE_DIR、OPENSSL_CRYPTO_LIBRARY和多架构include路径，精确路径与方法见SHA256.md；不声称在缺依赖的新目录中无配置即可构建。

## 验收矩阵

| 验收点 | 代码/测试证据 |
|---|---|
| 完整CLI→服务端文件一致 | upload_integration：实际子进程、空文件、二进制/中文路径、短末块和8个并发CLI；服务端文件逐字节一致 |
| 握手/严格消息/提交关联 | upload_control_test、既有协议测试，cli_upload_peer_test的10种正常/伪造/截断/源变更场景 |
| 实际块/整文件SHA-256 | UploadStorage::write/commit；错误块、错误整文件摘要均拒绝且不发布 |
| 安全文件与不覆盖 | TempFileStore、受限目录FD、renameat2(NOREPLACE)；路径、父级链接、已存在目标和提交前目标竞态测试 |
| 异步磁盘与背压 | DiskExecutor + UploadHandler；application_pause和capacity=1真实handler粘包重复提交回归 |
| ACK持久前提 | 具名数据及meta/目录先同步；pwrite完整→fdatasync数据→追加回执→fdatasync回执成功后返回；EINTR/短写/零写/ENOSPC/同步错误注入不返回ACK |
| 有界资源 | 64MiB文件/65536块/32保留session/512MiB预留数据、同根排他锁、连接/队列上限；quota和重启扫描回归 |
| 断连/重复 | 同块/同连接提交幂等，断连保留已确认数据，正常重启能接受新任务但不恢复旧会话 |
| 发布失败窗口 | rename后目录fsync故障：文件内容已校验，但不返回成功；COMMITTING状态启动拒绝，等待M3协调 |

## 失败与修正记录

- 新应用暂停测试先在空实现失败，再实现独立读暂停并通过。
- 初次旧binary不认识upload选项，端到端红测失败；联调修正COMMIT消息编号为指南的9/10。
- 严格提交协议首次观察的是缺header编译失败，未声称完成behavior-red。
- 独立审查发现启动打开FIFO会阻塞；子进程alarm回归先失败，改为O_NONBLOCK+同FD类型校验后三种FIFO均及时拒绝。
- 审查发现完成回调重入提交会在饱和时错误BUSY；容量1真实handler红测复现，改为queueInLoop延后恢复读取，再通过。
- 两项独立复核确认修复充分，未发现新的阻断问题。

## 未执行/未实现

无真实掉电、硬件磁盘满、完整kill -9恢复或TSan/性能基准证据；故障注入不等同真实硬件恢复测试。不支持SQLite重启续传、增量备份、下载或TLS/鉴权。部分发布/清理失败会使根目录在重启时保守拒绝服务，不能冒称自动修复。正式文件累计磁盘配额尚未实现，512MiB上限仅为未完成会话预留payload。

测试只在独立临时目录运行；未删除用户数据、未提交/推送GitHub，未运行新的远端GitHub Actions。

Qt原型窗口截图已实际检查，控件/说明/状态无裁切；无上传控件。UI运行假服务测试包含分片ACK、错误ID、错误字段类型、断连、超限、尾随字节和5秒超时。Qt配置缺可选Vulkan头的提示未影响Widgets/Network构建。

真实互通：Windows Qt --probe-port23587 → SSH本地转发 → Ubuntu /home/ning/ChunkHarbor/build/chunkharbor-server --handshake --port0（实际随机端口35087），GUI退出0。测试仅启动并清理自己记录的本地helper及远端服务PID，无真实备份数据。不能将此描述为GUI上传验收。
