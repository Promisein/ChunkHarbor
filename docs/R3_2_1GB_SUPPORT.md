# R3.2 1 GiB 支持实施记录

## 当前范围与基线

2026-11-05，本轮选择 A 的最小闭环，隔离实际函数 `DeltaStorage::finishManifest`（R3.1 以协议语义称 manifestDone）的基线读与摘要计算。A 其余长操作和 B/C/D 在后续子任务实施。R2 原标签为 9f60788，当前 HEAD 为 aa22a6f，已包含直接 TCP/SSH 与灵活备份范围。开工已有 SSH 管理的未提交改动，保留这些改动并以当前工作树运行回归。

## 修改前设计与验证步骤

1. 在现有 Linux LD_PRELOAD 故障库加入基线 pread 屏障，独立临时根运行真实服务。先用旧代码证明屏障期间另一连接请求超时。
2. 保持 finishManifest 接口。共享存储与 DB 锁下校验完整清单，固定不可变版本 FD，并复制最多 65,536 条哈希。退出锁作用域并销毁全部 SQLite statement 后，逐块读取、校验并生成有界缺失位图。工作仍归 DiskExecutor 线程。
3. 重新取得同一组锁，重载会话、核对基线/页数/阶段与预览配额。在单个事务内安装完整 missing 标记及 ranges，再更新 PLAN_READY。同一会话并发规划允许重复计算，已安装的结果由既有幂等分支返回。阶段已推进或会话过期则拒绝迟到安装。
4. I/O 错误只丢弃内存结果，会话保留 COLLECTING。事务失败回滚整份计划。FD 用 UniqueFd 持有，规划不跟随 current_files 变化，Exact/Absent 的 BEGIN 校验及 commit 冲突检查继续沿用现有逻辑。
5. 实际测屏障期间独立 BEGIN/QUERY、同路径版本更新，两个文件同时规划、重复 DONE、断连、SIGKILL、读取错误和会话过期。再运行 Linux 完整 CTest 与 Windows 构建和可运行回归。

原实现所有 Delta API 都用 std::scoped_lock 同时取得 u.mutex 与 u.dbMutex，没有独立 Delta 会话锁。旧上传另有会话 mutex。此次采用快照与重新校验，避免引入第三种锁及其获取顺序。内存上界由既有块数和工作池容量约束，哈希快照最多 65,536 个字符串，缺失位图约 8 KiB。SQLite 事务仍负责一次发布完整计划。

## 实际结果

本轮证据列于下文。

## 已完成的 A 子任务

finishManifest 现在按「固定输入、无共享锁计算、重新校验、原子安装」执行。规划仍使用现有 compareBlock 固定索引/长度/摘要规则。UniqueFd 在锁外持有旧不可变对象，即使 current_files 更新也不会换读新内容。同一会话允许并发重复计算；只有仍处于 COLLECTING 且输入一致的计算可以安装计划，已经 PLAN_READY 则复用既有应答。没有引入第三把锁、持久状态或新的协议字段。

哈希快照最多 65,536 个字符串，摘要文本净载荷约 4 MiB，另加 vector/string 分配开销；缺失位图约 8 KiB，正文仅保留一个受限块。空文件、块长变更、短末块沿用既有校验与完整回归。这个有界内存快照是本轮选择，后续若扩展块数必须重新评估。最终事务仍有 O(块数) 的 SQLite 更新，并且 SQLite 自身可能产生磁盘 I/O；本轮将基线正文读取及 SHA-256 移出了这段锁和事务。

修改范围为 src/storage/DeltaStorage.inc、tests/helpers/delta_crash_hook.cpp、tests/integration/delta_plan_lock_test.py、CMakeLists.txt 的单项 Linux CTest 注册，以及本页和 PROGRESS。CMakeLists 与 PROGRESS 叠加在已有未提交 SSH 改动之上，其他既有改动保留。

## 实际测试与证据

Linux 为 Ubuntu 22.04 / GCC 11.4，隔离源码与日志根为 /tmp/chunkharbor-r32-a-fI07y2Ac。测试数据由每场景新建的 /tmp/chunkharbor-plan-lock-* 根持有，临时根由测试清理。未打开正式工程的备份库。开发依赖复用 /home/ning/ChunkHarbor/build/deps 中的 OpenSSL 和 SQLite 头文件，仅输出到隔离根。

配置与编译命令如下。日志为 config.log、red-build.log、green-build.log、final-build.log。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug   -DCHUNKHARBOR_BUILD_GUI=OFF -DCHUNKHARBOR_BUILD_TESTS=ON   -DOPENSSL_INCLUDE_DIR=/home/ning/ChunkHarbor/build/deps/openssl/usr/include   -DOPENSSL_CRYPTO_LIBRARY=/usr/lib/x86_64-linux-gnu/libcrypto.so.3   -DSQLite3_INCLUDE_DIR=/home/ning/ChunkHarbor/build/deps/sqlite/usr/include   -DSQLite3_LIBRARY=/usr/lib/x86_64-linux-gnu/libsqlite3.so.0   -DCMAKE_CXX_FLAGS=-I/home/ning/ChunkHarbor/build/deps/openssl/usr/include/x86_64-linux-gnu
cmake --build build -j 3
ctest --test-dir build -R delta_plan_lock --output-on-failure
ctest --test-dir build --output-on-failure
```

- 旧实现红灯为独立 BEGIN 在 1 s socket deadline 内超时，red-test.log 保存该结果。测试搭建期间修正了 Python 局部变量遮蔽、连接会话绑定及错误响应后重连的夹具问题，之后才取得这项预期红灯。
- 新并发专项 8 组通过，final-lock-test.log，0.93 s。基线 pread 阻塞期间独立 BEGIN/QUERY 与同路径新版本发布共 0.024 s 完成，随后旧预览仍得到相对旧基线的正确差异。该数字只证明该次调度推进。
- 两个不同文件同时进入基线读取、同会话重复 DONE、Exact 旧版本冲突、同路径上传排他、断连后查询完整计划、SIGKILL 后从已持久清单重试、EIO 后保留 COLLECTING、安装事务触发器失败后无部分 missing/range 更新、会话过期拒绝迟到结果均实际通过。版本发布竞争测试用不同块几何完成更新，避免更新任务也被同一基线故障屏障暂停。
- Linux 完整 CTest **58/58**，42.88 s，full-test.log。包括原有协议、存储、迁移、历史、目录快照、故障和恢复测试。补强事务回滚断言后，再跑上述并发专项 **1/1** 通过。

## 未关闭项与后续顺序

A 整体仍未关闭。planPage 的复用块物化、query 的已确认块重验、commit/checkBase/publish 以及启动修复仍有长 I/O，下载打开时完整预哈希仍占用工作线程。固定 2 个 worker 同时承担两个长操作时也可能耗尽可用执行槽。本轮没有把全局服务可用性或取消时延视为已经解决。

下一项最小任务建议隔离 planPage 最后一页的复用块物化与共享锁，先明确同会话物化所有权、独占暂存、重入/重启与阶段提交语义，再以慢复制屏障验证其他请求推进。之后处理 query、commit 和长请求结果协调，完成 A/C 前置门槛，再进入 B 的空间会计及大小能力协商。

B、C、D 尚未实施，单文件仍为 64 MiB，全局声明大小配额仍为 512 MiB。1 GiB 上传/当前与历史下载/快照恢复、真实容量耗尽、GB CPU/内存/磁盘吞吐采样均未执行。没有改变 schema、检查点、帧大小或既有 HELLO 能力，也未提交或推送。MSVC、ThreadSanitizer、硬件掉电和 Linux Qt GUI 未验证。

Windows 11 / Qt 6.8.3 / MinGW 13.1 本轮执行以下命令，配置与增量构建退出 0，offscreen 非原生桌面 CTest **33/33**，111.50 s。日志位于 build-windows-gui/r32-config.log、r32-build.log、r32-test.log。配置提示缺可选 Vulkan 头；原生 ComboBox 专项本轮排除，未声称通过人工 GUI 或 SSH 业务验收。

```powershell
$env:QT_QPA_PLATFORM='offscreen'
cmake -S . -B build-windows-gui
cmake --build build-windows-gui -j 2
ctest --test-dir build-windows-gui -LE native-desktop --output-on-failure -j 2
```

## 2026-11-06 A 第二个闭环的修改前设计

本轮仅隔离 planPage 末页的复用物化。Impl 内增加活动物化会话集合，使用 RAII 租约在成功和异常时注销。集合始终在原 u.mutex/u.dbMutex 保护下访问，同会话并发末页返回既有 BUSY 错误，避免两次 O_TRUNC 破坏暂存。租约先于锁声明、在锁释放后销毁，避免析构重复获取自己持有的锁。

锁内生成 PlanDone、固定不可变基线和暂存 FD、复制最多 65,536 项复用索引/摘要。锁外执行 ftruncate、pread、块摘要核对、pwrite、文件与目录 fsync。重新加锁后重载会话并校验阶段、输入和基线，在事务内确认复用块 accepted 和 TRANSFERRING。失败保持 PLAN_READY，可重新物化；过期时不得安装迟到结果。重启丢弃进程内租约，沿用原 PLAN_READY 重做流程，无新 schema/协议状态。

新增真实服务端测试先得到预期红灯，慢基线读取期间独立 BEGIN 在 1 s 内超时。之后验证独立请求、重复末页、双会话、断连、SIGKILL、读取错误、ENOSPC/同步失败注入、事务失败和过期，保持旧版本摘要及最终文件正确。生产变更仅 Linux storage；Windows 运行兼容回归。工作区已有 SSH 与前一闭环改动继续保留。

### 本轮实际结果与资源语义

Linux Ubuntu 22.04 / GCC 11.4，隔离源码与构建根 /tmp/chunkharbor-r32-materialize-jRczwjCF，沿用上一闭环记录的 OpenSSL/SQLite 头与库参数，重新配置并执行 cmake --build build -j 3，退出 0。当前工作树（包括开工前已有改动）作为源码副本，未修改 Ubuntu 正式工程或启动正式备份库。

- 旧实现 red-test.log 实际失败于慢复制期间独立 BEGIN 的 1 s 响应超时。
- 改造后 ctest --test-dir build -R delta_materialize_lock -V 通过，10 组场景，1.01 s。屏障未释放前独立 BEGIN/QUERY 在约 2.7 ms 内返回，证明本次 worker 与共享锁允许独立请求推进。该数字不作为吞吐或普遍时延承诺。
- 覆盖正常复用、不同会话同时物化、同会话 BUSY、客户端断连后查询、SIGKILL 后 PLAN_READY 重新物化、EIO、ENOSPC 注入、fsync 错误注入、安装阶段事务触发器回滚、会话过期拒绝迟到安装。每次错误核对 accepted 总计为 0；可重试错误清除后在原会话成功。TRANSFERRING 后重复末页保留已经上传的缺块，最终发布内容正确，旧不可变版本逐字节一致。
- cmake 构建后 ctest --test-dir build --output-on-failure 完整 **59/59**，41.51 s，包含前一闭环规划屏障和原有迁移、断点续传、历史版本与快照故障测试。日志为 config.log、red-build.log、red-test.log、green-build.log、green-test.log、materialize-verbose.log、full-test.log。

有界复用清单最多 65,536 个索引/摘要字符串，正文始终一个块，目录和文件 FD 由 UniqueFd 持有。lease 声明早于这些资源，复制完成或异常退出时先释放 FD，再注销活动所有权，避免后续请求在旧 FD 尚写入时重新截断。租约是进程内状态，崩溃后仍按持久 PLAN_READY 重做。文件已刷盘与 SQLite accepted/阶段更新依旧按原顺序，不发提前成功应答。客户端断连时后台物化可能完成，随后查询返回真实阶段；取消时延仍由后续长任务治理处理。

本轮改动集中于 src/storage/DeltaStorage.inc 的租约和 planPage、测试故障库、独立 delta_materialize_lock_test.py、CMake 注册以及两份进度文档。保留此前 SSH 管理与 finishManifest 的未提交工作。

### 当前剩余门槛

A 整体仍未关闭。下一项最小任务为 query 的已确认块重验锁隔离，需要协调同会话 write/commit、失效块标记及固定暂存 FD，避免重验结果覆盖新的持久 ACK。之后处理 commit/checkBase/publish、启动修复与工作池长任务资源，达到门槛后再进入 B/C/D。

本轮沿用 64 MiB 单文件与 512 MiB 声明大小配额。未执行 1 GiB 实际上传/下载/历史与快照恢复、真实盘满或大文件资源测量。ENOSPC 和 fsync 错误是测试库注入。两 worker 同时长操作仍可能占满工作池；O_CREAT/O_TRUNC 的短设置操作和元数据/SQLite 事务仍使用共享锁，本轮隔离的是正文复制、校验及显式同步。协议、schema、检查点与对外 API 未改；BUSY 复用已有错误码，同会话并发末页收到它后需重连查询原会话。未执行 MSVC、TSan、人工 GUI、硬件掉电和长期压力测试。未 commit 或 push。

本轮 Windows 11 / Qt 6.8.3 / MinGW 13.1 执行 cmake -S . -B build-windows-gui、cmake --build build-windows-gui -j 2 均退出 0。设置 QT_QPA_PLATFORM=offscreen 后执行 ctest --test-dir build-windows-gui -LE native-desktop --output-on-failure -j 2，**33/33**，126.74 s。日志为 build-windows-gui/materialize-config.log、materialize-build.log、materialize-test.log。配置缺可选 Vulkan 头的提示不阻断构建。原生 ComboBox 专项本轮排除。测试数据仅在独立临时根；没有执行真实 1 GiB Windows→SSH→Ubuntu 业务链路。git diff --check 及新测试的 Python 编译检查通过。

## A.3 修改前方案（2026-11-06）

旧 query 在 u.mutex 与 u.dbMutex 下遍历全部 manifest_chunks，逐块 pread 暂存、计算 SHA-256 并清除失效 accepted。没有显式事务，单条失效更新可触发 SQLite 自动提交。确定性暂存 pread 屏障已复现独立 BEGIN 的 1 s 超时。

复用 A.2 的 RAII 所有权集合，泛化名称为 SessionIoLease/sessionIo。查询仅在 TRANSFERRING/VERIFYING 时占用同会话 I/O 租约，锁内固定 FD 和块元数据，锁外重验，再加锁重新加载会话并在事务内撤销失效 accepted。重验期间同会话重复 query、write、commit 返回既有 BUSY；没有引入会话 mutex 或新的锁顺序。非传输状态查询继续直接返回原有状态。

事务内只允许把 accepted 从真改假，绝不升级未确认块；租约阻止同会话写入或提交与旧重验结果交错。过期清理可以 unlink 暂存，但 FD 固定 inode，最终 load 失败会拒绝迟到结果。断连不撤销已确认状态；后台工作安全完成后丢弃失效连接回调。缺失暂存保持原错误，短读/EIO/坏摘要保守视为缺块，允许重新传输。对外函数、帧、检查点及 GUI 保持既有格式。

## A.3 续传状态查询审计与隔离（2026-11-06）

### 调用链及锁的原状

GUI BackupWorker 从本地 checkpoint 校验端点、源身份、会话和基线后，以独立 Peer 发 QUERY_DELTA(28)；共享 client_core::runDelta 的 resume 分支同样编码会话 ID，解码 DELTA_STATUS(29)，验证 session/mode/base，再处理阶段和 missing bitmap。Linux/Qt CLI 的 DeltaCommand 读取持久检查点后复用 runDelta。提交成功但应答丢失时，COMMITTED 状态及原 result 仍是确认入口。

UploadHandler 在所属 EventLoop 上校验能力与连接绑定，暂停该连接读取，将 request 交给有界 DiskExecutor。worker 调用 DeltaStorage::query；结果安全投递回 EventLoop，编码 DELTA_STATUS。错误按原 UPLOAD_ERROR 返回并结束连接。闭包使用存储引用，服务退出会先 join worker 再销毁存储；完成回调只持 weak connection，断连后的结果不会写失效连接。

Delta query 原来用 std::scoped_lock 同时取得 u.mutex/u.dbMutex，直到所有 SQLite 行遍历、暂存 pread、块 SHA-256、accepted 清除和位图转换完成才释放。读取对象为 .delta/<session>，不读历史版本对象。已确认块总字节可接近文件大小，遍历和位图成本随块数增长。没有显式文件 fsync，但失效 accepted 的逐条 UPDATE 触发自动事务，WAL/FULL 仍有持久写成本。COLLECTING、PLAN_READY、COMMITTED 的查询没有这段正文重读。

兼容旧 upload(QUERY_SESSION 7) 经 UploadStorage::query，在短全局查找后取旧 UploadSession::mutex，只遍历内存 accepted 位图，不在 query 内重读/哈希正文。启动恢复仍重验块，本轮保留此路径。Delta 没有独立会话 mutex；A.2 租约也由原两把锁保护。本轮没有增加锁层次。

客户端取消在本地停止请求/断开连接并保留检查点，当前协议无单独 Delta CANCEL/DELETE。过期路径包括 load 的状态过期以及 begin/启动 expire 的元数据和暂存清理。query 与同会话 write/commit 原靠共享锁串行；若只解锁读文件，迟到的坏块结果就可能撤销新 ACK，提交也可能移除暂存，因此必须同时协调这些入口。

### 改造与生命周期

本轮把 A.2 的 MaterializationLease/materializing 泛化为 SessionIoLease/sessionIo，保持同一套 RAII 注销和锁顺序。planPage 继续使用它。TRANSFERRING/VERIFYING 查询在锁内取得租约、固定 UniqueFd、快照最多 65,536 条有序 idx/sha/accepted；全部 DStmt 在解锁前销毁。锁外仅用局部快照和 FD 逐个重验已确认块，正文缓存一个块，位图最多 8 KiB。idx 连续性和条目数量会核对，避免不完整元数据产生虚假的已确认位图。

查询重验期间，重复慢 query、同会话 write 和 commit 在原锁内检查租约并返回已有 BUSY 错误码。没有新增锁等待或无限重试。普通元数据状态查询与独立会话仍按原流程执行。write/commit 先获得共享锁时会先完成操作，再由 query 读取新状态；commit 本身的长持锁尚待下一闭环。

读取错误、短读或坏摘要继续保守视为缺块。缺失暂存导致打开失败，保持原 INTERNAL_ERROR。查询不把任何未确认块升级为 accepted。返回前重新取得两把锁、load 会话、校验阶段/元数据/基线/页数，在单个事务内撤销失效 accepted；失败整体回滚并报错，避免返回未成功更新的状态。无失效块时不新增写事务。

UniqueFd 固定 inode，过期清理可以 unlink 路径，但不会使正在读的 FD 悬空。重新 load 已过期会话时拒绝迟到结果，不重新创建会话或元数据。局部 FD 先于 lease 释放，scope 中的锁先于 lease 析构，避免递归加锁或过早让新操作进入。客户端断连只影响回调投递，查询本身仍安全收尾。会话租约不持久化，崩溃后依靠原 SQLite 状态和启动重验恢复。

BUSY 是本轮有意选择的最小同会话并发策略。当前客户端将它作为失败收尾，随后需要重新连接并查询原会话；本轮没有自动重试或 GUI 行为变更。后续提交锁隔离必须复用这套所有权，防止提交解锁后新查询/写入交错。

### 新测试与结果

新增 tests/integration/delta_query_lock_test.py，复用现有协议探针和有界等待函数。故障库只在设置 CH_QUERY_TEST_GATE 且 FD 属于选定临时 .delta 会话时阻塞 pread，通过 entered/release 文件协调线程，不用固定 sleep 作为竞态发生证据。轮询有明确 deadline。脚本所有数据都在独立 chunkharbor-query-* 临时根，未打开真实备份库。

场景包括正常查询、完成后重复查询、慢查询时重复查询的 BUSY、同会话写入和提交竞争、客户端断连、SIGKILL 后恢复查询、缺失暂存后恢复合法权限文件、坏块、EIO、load 过期、expire 实际 unlink/删行、失效更新事务触发器失败回滚。可恢复场景最后重新上传缺块、提交并查询 COMMITTED；检查位图不会把未确认块报告为已确认。缺失暂存夹具首次重建时因默认权限不是 0600 而被安全检查拒绝，修正夹具权限后通过，产品路径约束未弱化。

Ubuntu 隔离源码与构建根为 /tmp/chunkharbor-r32-query-3Zq8GCWy，依赖路径同上两轮。配置命令及所有输出保存在 config.log、red-build.log、red-test.log、green-build.log、green-test.log、query-verbose.log、full-test.log。旧实现实际红灯为暂停查询 pread 后，独立 BEGIN 在 1 s socket deadline 内超时。生产代码修改前取得此证据。

### A.3 最终验证与剩余事项

Ubuntu 22.04 / GCC 11.4 配置和编译退出 0。实际执行 cmake --build build -j 3、ctest --test-dir build -R delta_query_lock -V，专项 **12 场景通过**，0.77 s；屏障释放前独立 BEGIN/QUERY 在该次运行约 2.28 ms 完成。随后 ctest --test-dir build --output-on-failure，完整 **60/60**，42.86 s，含 A.1/A.2 和原 R1/R2 存储、版本、迁移及恢复回归。

Windows 11 / Qt 6.8.3 / MinGW 13.1，cmake -S . -B build-windows-gui 与 cmake --build build-windows-gui -j 2 均退出 0。设置 QT_QPA_PLATFORM=offscreen 后执行 ctest --test-dir build-windows-gui -LE native-desktop --output-on-failure -j 2，**33/33**，124.55 s。日志为 build-windows-gui/query-config.log、query-build.log、query-test.log。配置有缺可选 Vulkan 头提示。git diff --check 与新脚本 Python 编译检查通过。

本轮修改为 DeltaStorage.inc 的租约泛化、query 快照/重验/重校验以及 write/commit 的同会话 BUSY 保护，故障库的定向暂存读屏障、delta_query_lock_test.py、CMake 注册和两份开发记录。开工前 SSH/GUI 等工作保留，本轮没有追加 Qt GUI、协议格式、schema、文件限制或配额变更。

可以在下一轮进入提交阶段锁隔离审计。A 仍有 commit/checkBase/publish 的整文件读哈希/复制/同步与全局锁、启动恢复的全量校验、部分 begin legacy 协调。提交先持锁的方向仍会让查询等待，不能把查询先持租约的隔离当作提交已隔离。query 本身的 O(块数) 元数据快照和失效 accepted 事务仍在锁下，SQLite 提交仍可能等待磁盘；两 worker 同时处理不同长任务也仍可能占满执行槽。

没有运行原生桌面专项、真实 Windows→SSH 业务端到端、1 GiB 样本、真实磁盘耗尽、TSan/MSVC、掉电或长时间资源压测。GUI 主动取消沿用既有取消/断连机制，本轮以断连及过期清理注入验证服务端生命周期，没有新增远端取消命令。单文件仍为 64 MiB，配额仍为 512 MiB，A 整体尚未关闭，不进入 B。未 commit 或 push。

## A.4 提交路径修改前审计与方案（2026-11-06）

客户端 runDelta 发送 COMMIT_DELTA(30)，UploadHandler 在 loop 校验绑定并交 DiskExecutor，调用 DeltaStorage::commit，完成后以 DELTA_RESULT(31) 回复。客户端丢应答后可按原 session 查询 COMMITTED/result。旧 commit 在整个函数持有 u.mutex/u.dbMutex；只检查 A.3 的 sessionIo，不主动取得租约。

锁内依次校验 phase/缺块、重读暂存整文件摘要、checkBase 的 current 指针和当前文件整文件摘要、生成或核对不可变对象、同步对象及版本目录。第一写事务插入版本与块清单、记录 phase=4。publish 再读对象和当前副本、复核基线、完整复制 .publish 临时文件、同步、rename、同步父目录，然后第二写事务更新 current_files（触发历史记录）与 phase=5/result。最后释放逻辑路径预留并清理暂存。SQLite 事务没有覆盖所有文件 I/O，但全局锁覆盖。旧 upload 则用自己的 session mutex，在短 dbMutex 下登记状态，路径预留与 Delta 共用。

拟复用 SessionIoLease，并为活动提交增加单独的过期保护集合（不改变 A.3 的查询过期语义）。锁内固定 FD/基线元数据，锁外哈希和对象复制，重新校验后才登记 COMMITTING。publish 与启动修复共用分段实现，在锁内核对数据库指针及当前 inode/大小/时间身份后 rename；目录同步在锁外，最终事务再次核对指针和文件身份，确保没有未同步对象被标成完成。已有上传路径预留持续阻止同路径另一上传；Exact/Absent 及最终版本复核全部保留。

同会话写入、查询和重复提交复用 BUSY；COMMITTING 无活动工作时允许原 session 重新执行安全发布。事务失败保留旧 phase 或 COMMITTING，恢复继续沿用现有对象、版本 ID 与幂等结果。不增加 schema/帧/文件上限或配额。本轮确定性测试先复现慢提交 pread 时独立 BEGIN 在 1 s 内超时，随后覆盖不同 I/O 屏障、生命周期、SQL 与文件故障，并复用原 SIGKILL 发布窗口回归。

### A.4 最终实现与锁／事务边界

本轮使用同一套 SessionIoLease 保护 commit 的整个工作寿命。锁内登记 sessionIo 与 pinnedCommits 后，独立会话不受这份租约限制；同会话 write、query、commit 竞争返回已有 BUSY。COMMITTED 的幂等只读返回仍可取原 result。expire 和 load 在活动提交期间跳过过期处理，租约析构在共享锁作用域之外注销两份集合，不增加会话 mutex。查询/物化原有过期语义继续保持。

| 步骤 | 共享锁与事务 | 对象与慢操作 |
| --- | --- | --- |
| 会话准备 | std::scoped_lock 同时取得 u.mutex/u.dbMutex；校验阶段、缺块和租约，固定暂存及基线 FD、父目录句柄、版本元数据 | 只做受控路径打开和元数据读取，暂存由租约防止同会话修改 |
| 数据与对象准备 | 释放两把锁，无 SQLite statement/写事务跨越 | 暂存整文件 SHA-256；固定当前文件的基线校验；版本对象复制、同步、rename 及版本目录同步 |
| 登记 COMMITTING | 重新取得两锁，重载会话并复核基线；BEGIN IMMEDIATE 中写版本、块清单和 phase=4，COMMIT | 数据对象已完成约定同步，current 指针尚未更新；事务失败整体回滚 |
| 发布准备 | publish 自行取得两锁读取持久状态，固定不可变对象、当前文件与目标父句柄，然后释放 | 重验对象及当前内容，物化并同步 .publish-<id> |
| 当前命名空间切换 | 重新取得两锁，复核 current 版本及当前文件身份，执行 rename/RENAME_NOREPLACE | namespace 切换仍在锁内；文件 inode/dev/size/mtime/ctime 必须匹配，防止解锁间被替换 |
| 发布持久化 | 释放两锁 | 同步目标父目录及 staging 目录，成功前不进入完成事务 |
| 完成元数据 | 两锁下复核文件身份；BEGIN IMMEDIATE 再核对 current，更新 current_files、history 触发器及 phase=5/result，COMMIT | SQLite/WAL 同步依旧由事务保证，未将持久确认提前 |
| 清理与应答 | 锁内释放路径预留；锁外清理本 session 暂存并同步；RAII 回收 FD 后注销租约 | 已提交后发生应答丢失或清理错误，重复提交仍返回原 result，原 committed 状态不被回退 |

BaseSnapshot 把 checkBase 的元数据快照与 verifyBase 的正文读取分开。初始验证及最终 namespace 切换前都核对版本。原 u.paths 路径预留持续存在，两个正常客户端无法同时创建同路径的活动上传；第二个会在 BEGIN 被拒绝，第一份提交后使用旧 Exact 的新 BEGIN 也被拒绝。没有为制造竞争而放宽该规则。专项另用受控 SQLite 指针注入及同内容 inode 替换验证两次最终复核，迟到结果不会覆盖新指针或新文件。

publish 与启动恢复共用这份分段实现，避免两套发布逻辑。COMMITTING 无活动工作时，原 session 的重复 commit 可以继续 publish；query 对该状态仍保持 BUSY。启动时原 phase=4 扫描依旧执行 publish，在没有网络 worker 的构造阶段完成恢复。状态机枚举、协议字段与 schema 均未扩展。版本历史仍由 current_files 事务内触发器登记，快照和下载在中间窗口看到旧指针时读取旧不可变对象，不会读取尚未完成的新版本。

SQLite 与文件系统仍不组成单一事务。对象准备但第一事务未成功时，不注册 current/history，沿用启动 isolateObjects 隔离无引用对象的策略。phase=4 已持久但当前文件未发布时重做安全复制；rename 成功但最终 SQL 失败时保留 phase=4、旧 current/history 和完整新对象，原路径保持预留，重启或同会话重试补齐元数据。最终 SQL 成功后以原 result 确认结果，不回退提交。

### A.4 验证

本轮全部 Linux 数据使用独立 chunkharbor-commit-* 临时根，源码与构建复制到 /tmp/chunkharbor-r32-commit-oG6De902。未打开正式备份库、未部署到正式服务。Ubuntu 22.04/GCC 11.4，沿用本页前述 OpenSSL/SQLite 私有头路径，cmake 配置与 cmake --build build -j 3 均成功。实际命令包括以下各项。

```bash
ctest --test-dir build -R 'delta_commit_lock|delta_recovery|delta_fault|delta_storage' --output-on-failure
ctest --test-dir build -R delta_commit_lock -V
ctest --test-dir build --output-on-failure
```

新增 delta_commit_lock_test.py 最终 **26 场景通过**，2.76 s，commit-final.log。故障库定向匹配 fd 路径与 pread/pwrite/fsync，entered 文件证明进入慢操作，release 文件决定放行。暂存哈希、基线哈希、对象复制、发布复制、对象同步及目标目录同步六类屏障中，独立 BEGIN/QUERY 均在屏障释放前完成，该次观察约 2–4 ms；不把这个数值当作性能指标。

覆盖正常与重复提交、同会话 query/write/commit 的 BUSY、不同路径同时提交、同路径预留和过期保护、断连、复制 ENOSPC、同步 EIO、全文件摘要错误、准备 SQL 回滚、rename 已成功而最终 SQL 回滚、原 session 在线重试 COMMITTING，以及最终指针/目标 inode 竞争。对故障同时核对真实 files 内容、不可变旧对象、版本行、history、current、phase、暂存和恢复后清理。失败复制留下的 pending 对象、第一事务回滚后的无引用对象由启动隔离目录接收，专项已核对。没有在测试中禁用校验或同步。

应答丢失专项使用测试 socket 包装器接收真实 DELTA_RESULT 成功帧后丢弃，只向客户端暴露 EOF；随后按同 session 查询与重复提交确认同一结果、同一版本及唯一 history 行。这是本机可控对端注入，未宣称真实 SSH 网络丢包。两个新增 SIGKILL 场景分别在对象复制和发布临时复制的屏障内终止服务；原 delta_recovery_integration 另覆盖对象已同步、rename 前、rename 后、COMMITTED 清理后的四个 SIGKILL 窗口，均通过。

完整 Ubuntu CTest **61/61**，44.24 s，full-test.log。随后补强最终发布指针与 inode 竞争、内容损坏和在线重试断言，最终专项再次 **1/1**，含上述 26 场景。源代码相同，包含 A.1/A.2/A.3 及 R2 历史恢复、目录快照、schema 迁移和故障回归。配置/编译与红绿日志保存在隔离根的 config.log、red-build.log、red-test.log、green-build.log、green-test.log、commit-verbose.log、commit-final.log、full-test.log。旧实现红灯为慢 commit pread 期间独立 BEGIN 的 1 s 超时。

Windows 11/Qt 6.8.3/MinGW 13.1，cmake -S . -B build-windows-gui、cmake --build build-windows-gui -j 2 均成功；QT_QPA_PLATFORM=offscreen 下执行 ctest --test-dir build-windows-gui -LE native-desktop --output-on-failure -j 2，**33/33**，124.11 s。日志为 build-windows-gui/commit-config.log、commit-build.log、commit-test.log。配置仍有缺可选 Vulkan 头提示。未运行原生桌面专项、Linux Qt GUI、MSVC/TSan、真实 SSH 业务链路、硬件掉电、真实盘满或 GB 资源测试。

### A.4 验收判断与 A.5

A.4 在当前 64 MiB、单进程单服务、可信根目录使用边界下达到本轮验收条件。生产改动集中于 DeltaStorage.inc 的提交/基线/发布拆分与活动提交过期保护；其他改动为故障库、CTest 注册、新专项和两份文档。已有 SSH/GUI 及 A.1–A.3 未提交改动保留。文件上限、512 MiB 配额、Qt GUI、协议格式和 schema 本轮均未修改。未 commit/push。

A 整体仍未关闭。A.5 应继续审计启动最新文件/接受块重验、legacy 协调、过期批清理和普通元数据路径的锁成本、SQLite/WAL 与块清单事务的持锁时间、工作池两个长任务同时占满的服务能力，以及失败后残留对象的长期资源上界。运行时对象准备失败仍可能留下无引用对象，沿用启动隔离策略，没有新增运行时回收。rename、目录打开等元数据操作及 SQLite 事务仍在共享锁下，不能保证它们在慢盘上的时延上界。新旧协议共用的旧 UploadStorage 提交仍由独立会话 mutex 管理，相关同步成本保留审计。安全边界继续排除同属主主动篡改和真实掉电保证；注入测试不足以替代这些验证。

## A.5 审计及修改前方案（2026-11-06）

当前 main 先构造并绑定 TcpServer socket，再执行 UploadStorage/DeltaStorage 恢复；此时 EventLoop 尚未运行，worker 及连接回调尚未创建，无客户端业务并发。phase=4 复用 A.4 publish，随后核对最新文件与 accepted 块。失败发生在 LISTENING 前，需保留阶段并提供 session 上下文。无需把启动全量校验迁移成后台任务。

已用新测试复现旧 phase=7 行被反复扫描、再次清除逻辑路径预留，导致同路径第二个 BEGIN 可能成功。另发现 A.1–A.3 只有 commit 被 pinnedCommits 保护，活动重验/物化会被过期清理打断；成功元数据提交后清理失败也缺少明确的重启暂存清理路径。

拟把活动 pin 泛化为计数，A.1 允许重复规划而每个规划各持一份 pin，A.2–A.4 继续同会话独占 I/O。任务结束后 pin 归零，下一次访问/清理照常执行 TTL，不延长持久租期。清理先保留 phase=7，完成已知暂存与 .publish 文件删除/同步，再事务性删除辅助元数据和失效会话，失败保留可重试状态；只有没有其他活动 Delta/legacy 使用者时释放路径。COMMITTED 仅回收冗余传输清单和暂存，保留会话 result、版本清单、history/current/snapshot，支持重复查询和提交。

工作池现为 2 worker/64 outstanding，包含队列和完成回调，默认最多 32 个上传连接。Full/Stopped 立即拒绝；worker 不等待 Reactor 回调，stop 先 join 后 drain。补确定性两 worker 饱和测试，观察 Reactor 仍可执行排队回调、第三任务有界等待、容量拒绝和完成释放。

准备事务回滚留下的随机版本对象没有持久 session 所有权，重试会生成更多对象，启动只隔离到 quarantine。不能仅按“无引用”删除，尚需可验证的失败对象归属/重试复用机制；本轮用重复故障与重启量化该实质阻塞，不以现有测试通过关闭 A，也不扩展为历史版本 GC。

### A.5 审计后的实际修改

1. pinnedCommits 泛化为 activePins 计数，RAII pin 覆盖 A.1–A.4。重复规划各有 pin，但不误删另一物化/查询的独占 sessionIo；不同任务结束按计数释放。操作开始时已过期的会话仍立即拒绝。操作进行中越过 TTL 时，清理延后到最后一个 pin 释放；下一次访问或 begin/startup 维护按原创建时间过期，没有刷新租期。
2. load 只标记 EXPIRED，不立即释放路径；expire 在保留 phase=7 的条件下删除已知 session 暂存与 .publish 文件并同步，再事务性清除辅助记录和过期会话。文件或 SQL 失败保留可重试状态。旧 phase=7 行不会反复抹掉新会话的路径预留；释放路径前还会核对其他活动 Delta 与 retained legacy 任务。
3. COMMITTED 后由 cleanupCommitted 回收本任务暂存和冗余传输清单，保留会话 result 和版本/history/current/snapshot。重启清理完成但残留的暂存，按旧迁移约定保留既有数据库行。首次全量测试中，启动清除旧 manifest 行使两个真实迁移夹具失败，已修正这项兼容问题，未削弱迁移断言。清理失败仍保持 COMMITTED，可通过状态确认结果并在重启重试清理。
4. 为 COMMITTING 恢复和已提交暂存清理补充 session ID 级错误上下文。恢复失败退出且不打印 LISTENING 就绪，不对客户端提供半恢复状态。修复故障后可重复启动。端口在恢复前已绑定，TCP 连接成功不等于业务就绪，正式判断仍应使用 HELLO。
5. 工作池生产实现保留。新确定性单测同时占用两个 worker、排队第三个任务，确认第四个 Full、outstanding 包含等待/执行/待回调，Reactor 心跳可释放屏障，最终所有回调在 loop 线程完成并清零。原单测继续覆盖 Stopped、stop/drain、析构取消未投递回调和完成传输独立于 loop 普通队列。

### A.5 恢复与资源生命周期

| 状态/资源 | 保留及恢复规则 | 当前边界 |
| --- | --- | --- |
| COLLECTING/PLAN_READY | SQLite 清单/计划保留，物化中断可按旧状态重做；活动 pin 结束后才允许过期 | 控制页和块数仍有界，TTL 不刷新 |
| TRANSFERRING/VERIFYING | staging/accepted 保留供续传；启动重验坏块并清除确认 | 正文校验在启动就绪前串行执行，启动耗时仍随数据规模增长 |
| COMMITTING | 固定版本对象和 phase=4，复用 publish 补齐 FS/DB 状态 | 不按普通 TTL 删除；持续故障会阻止就绪，需排除故障后再重启/重试 |
| COMMITTED | 保留幂等 result、不可变版本及历史/快照引用 | 正常新提交回收传输清单；启动只回收暂存，旧记录和历史保留策略属于 R4 |
| EXPIRED | 活动 pin 归零后删除已知 staging/.publish 与传输清单、session，再释放自己的路径 | 删除失败或事务回滚保留 phase=7，后续 begin/startup 可重试 |
| legacy 上传 | 旧协议/CLI 和旧备份迁移仍需要。session mutex 管正文，短 dbMutex 更新状态 | 32 个会话及 512 MiB 预留限制内保留断连会话；旧会话 TTL 主要在重启恢复时执行，运行时不能承诺即时回收 |
| 未登记的准备对象 | 当前启动将无 versions 行的目录隔离到 quarantine | 没有持久 session 归属，随机新 ID 重试造成增长。隔离不是回收，A 关闭阻塞 |
| FD/租约 | UniqueFd、局部快照和 RAII pin，任务退出释放 | 重复取消/续传/提交专项观察 FD 回落；没有 TSan 或长时间系统压力结论 |
| 队列/回调 | 2 worker、64 outstanding，默认 32 连接；单连接一次一个磁盘请求 | 两个长任务占满后普通磁盘请求排队，Reactor 继续运行。stop 等待有限工作并 drain，不依赖 loop 回调才能结束的 worker |
| 已发布版本/快照 | 含历史引用的对象全部保留 | 生命周期可随成功备份增长，属于 R4，不能当作孤立对象删除 |

过期清理的 unlink/fsync、辅助元数据删除事务仍在共享锁内，以保证现有路径预留和失败重试顺序；它没有正文扫描，但慢盘可产生较长延迟。没有宣称所有 metadata syscall 都有时延上界。SQLite 单连接仍以 WAL/FULL/foreign_keys=ON 工作。

### A.5 元数据与持锁分类

| 类别 | 当前路径与判断 |
| --- | --- |
| A，原子性要求保留 | current/history/result/phase 的最终事务、版本行与块清单的准备事务、命名空间切换前复核、失效 accepted 原子撤销。事务仍可能等待 SQLite 同步，不能任意把关键状态拆开。 |
| B，仍有可隔离正文工作 | 运行时 legacy() 在 Delta BEGIN 的全局锁下核对旧当前文件/版本对象并生成旧块索引。新专项已用屏障证实独立请求阻塞，不作为已解决项。 |
| C，可进一步缩范围 | 过期批扫描、unlink/fsync、批量辅助行删除、begin 扫描累积会话及 previewBytes 聚合。当前保证一致性，尚无大规模时延测量。 |
| D，当前合理 | 无正文 I/O 的会话绑定、能力检查、短状态/版本结果读取及有界位图编码；无需为消除所有锁而重构。启动串行恢复无在线共享锁竞争，优先保持可靠性。 |

metadata 表随保留历史增长而变大，BEGIN 的全表会话扫描及 legacy 全量已提交查询也会受影响。这些成本没有被本轮少量文件测试证明为长期有界。工作池只有数量界限，没有强制磁盘操作完成期限；永久卡住的内核 I/O 会延迟关闭，这属于剩余可用性风险，不能等同于 Reactor/worker 的锁顺序死锁。

### A.5 专项与综合证据

Ubuntu 隔离源码及日志根为 /tmp/chunkharbor-r32-a5-DCfOYFL2，Ubuntu 22.04/GCC 11.4，配置使用前述 OpenSSL/SQLite 私有依赖路径。cmake 配置和 cmake --build build -j 3 退出 0。测试数据全部为 chunkharbor-a5-* 隔离根；未部署正式工程或打开真实备份库。

实际命令如下。

```bash
ctest --test-dir build -R delta_lifecycle -V
ctest --test-dir build -R 'delta_.*lock|delta_lifecycle|disk_executor' --output-on-failure
ctest --test-dir build -R 'directory_snapshot_integration|version_history_integration|delta_lifecycle|disk_executor' --output-on-failure
ctest --test-dir build --output-on-failure
```

新生命周期专项 **12 场景**，1.26 s。包含两个用来确认开放阻塞的审计场景，其 PASS 表示问题已按预期复现，不能解释为相关能力已验收。具体证据如下。

- 旧 phase=7 行影响新路径预留的回归先红灯，修复后第二个同路径上传被拒绝，旧行被清除，新行保持原状态。
- unlink 失败与 cleanup DELETE 事务触发器失败，均保留可重试 phase=7；清除故障后原文件和 .publish 清理，行删除成功。SQL 失败前删除的辅助行会回滚。
- 已提交清理失败后状态仍为 COMMITTED，启动故障日志含 session ID；取消故障后启动清理成功，旧数据库清单行保留。
- A.1/A.2/A.3 活动时人为推进租期并运行清理，任务仍可完成且文件不误删；pin 结束后再次访问会过期，下一次维护完成回收。原有过期专项同步改为这项明确语义，保留了后续拒绝过期会话和删除资源的断言。
- COMMITTING 的最终事务失败造成两次启动失败，日志均含会话 ID，phase=4 保持；修复触发器后成功恢复。
- 连续 8 轮上传、断连、查询续传和提交，正常路径无 staging 残留，FD 回落，随后连续三次重启得到同一 committed result，integrity_check/foreign_key_check 正常。历史对象数量按成功版本保留，不能把其增长当作失败垃圾。
- 同一会话三次准备事务回滚新增 **3 个未登记版本对象**，重启后 **3 个目录转入 quarantine**。代码每次选新随机版本 ID，没有按 session 持久关联准备对象；因此当前确有失败重试导致的线性磁盘增长。
- 真实 legacy 上传后触发 Delta BEGIN 的 legacy 索引，固定版本 pread 屏障进入后，另一个独立 BEGIN 在 0.3 s 内超时，释放屏障后可完成。此时两个 worker 中一个持锁、一个等待锁，区别于两个磁盘任务已占满的工作池排队。

关键 A.1–A.4、生命周期与工作池组合 **6/6** 通过。首轮完整 CTest **60/62**，失败为旧迁移记录保留要求；修正启动压缩行为后相关 **4/4**，最终完整 **62/62**，45.44 s。包括历史恢复、目录快照、旧 schema 夹具、发布 SIGKILL 与各锁隔离专项。日志为 config.log、red-build.log、red-test.log、green-build.log、green-test.log、final-build.log、focused.log、full-test.log、migration-fix.log、lifecycle-final.log、full-final.log。

Windows 11/Qt 6.8.3/MinGW 13.1，cmake -S . -B build-windows-gui、cmake --build build-windows-gui -j 2 成功；QT_QPA_PLATFORM=offscreen 下 ctest --test-dir build-windows-gui -LE native-desktop --output-on-failure -j 2，**33/33**，117.73 s。日志为 build-windows-gui/a5-config.log、a5-build.log、a5-test.log。最终 Linux 专属修复不改变 Windows target 源码。git diff --check 和 Python 编译检查通过。

没有运行真实 SSH 业务端到端、TSan、原生桌面专项、真实硬件掉电、真实盘满、1 GiB 或 4–10 GiB 文件测试。连接 Ubuntu 执行隔离 CTest 不等于 Windows GUI→SSH 数据传输验收。

### A.5 与 A 阶段结论

**本轮完成 A.5 审计、过期/残留清理的最小修复及综合测试，A 阶段尚不满足关闭条件。**

| 关闭条件 | 当前结论 |
| --- | --- |
| A.1–A.4 主要 I/O 隔离 | Delta 主路径专项通过；legacy 在线索引仍是实质阻塞。 |
| 独立请求推进 | Delta 四类慢操作通过；legacy 索引屏障反例已复现。 |
| 同会话安全 | 租约、BUSY、活动 pin 与过期后回收通过。 |
| 提交/恢复幂等 | 原发布故障、失败恢复与连续三次重启通过。 |
| 不误删活动文件 | 计数 pin 覆盖 A.1–A.4，清理与 pin 使用原两锁，无新增锁顺序。 |
| 失败残留有回收界限 | 不满足。未登记对象持续增长，移入 quarantine 不等于回收。 |
| 工作池有界且无相互等待 | 队列、完成容量和 Reactor 推进测试通过；不保证慢 I/O 或排队的固定时延。 |
| current 与不可变对象一致 | 相关发布、故障、历史和快照回归通过。 |
| 两平台回归 | Ubuntu 62/62、Windows 非原生 33/33。 |
| 未验证风险记录 | 已列明，不能用全绿 CTest 覆盖上述反例。 |

下一项最小工作应先设计并验证准备对象的持久归属与同会话重试复用，再定义故障及过期后的精确回收条件。不能在线扫描后删除所有无数据库引用目录，因为其中可能包含另一个 worker 正在准备的对象、待重试对象或旧恢复数据。对已经存在且归属未知的 quarantine 内容应继续保留并报告，不借本轮开展历史 GC。之后隔离 legacy 注册的正文读取与长事务，保留旧协议和夹具迁移语义。两项关闭证据补齐前，不进入 R3.2 B。

本轮源码改动为 DeltaStorage.inc 的活动 pin、过期清理、已提交暂存清理及恢复诊断；测试改动为定向 unlink 故障、新生命周期专项、过期语义回归和两 worker 饱和用例，另有 CMake 注册与两份文档。旧版本对象和有历史/快照引用的数据均保留。文件大小、配额、协议、schema、Qt GUI 和固定分块算法均未调整，既有工作区改动保留，未 commit/push。

## A.5.1 持久准备归属方案（2026-11-06）

原问题在改动前再次复现，会话 19aa9ed4a3b861d75c3f10b7b4aa9005 保持 phase=2、version_id=NULL。对象 55025376dc1516e43534f4d605de5fcf、83fd4bbaca72e0fff437d201d09da723、710ca3342a41a807a84603975955672c 各 8 字节，SHA-256 均为 39b42cf74def0be3e9e02e5552d9dc79b393452c7816d43ff9d59caf66eb54b6，versions 行均为 0，重启后三个目录进入 quarantine。证据保存于本轮隔离根 baseline-repro.log。

现有 delta_sessions.version_id 外键指向 versions，且当前仅在对象准备事务中登记；将归属放在同一事务会随回滚消失。无需为此增加 schema，本轮选择独立目录 .preparations 的小型原子日志，每条以 session ID 命名，含格式版本、会话、随机对象版本 ID、完整 BeginDelta 请求及 preparing/ready/reclaiming 状态。日志通过临时文件写入、文件 fsync、rename、目录 fsync 独立持久化，然后才允许创建对象目录。对象完成原同步步骤后记录 ready，再进入原 SQLite 准备事务。

同会话在有效租约下复用同一日志/对象 ID。重启先加载和校验日志，再恢复：已登记 versions 且与同会话 COMMITTING/COMMITTED 对应的对象保留；无引用的有归属准备对象先记 reclaiming，再只删除该目录中受控的 pending/data，逐级同步，最后删除日志并同步。上传暂存、accepted 和会话仍保留，可继续查询与提交。过期清理在删除 session 前先回收其归属对象。旧隔离对象无日志证据，继续保留。

回收与版本登记使用既有两把锁和活动 pin，恢复发生在 worker 业务派发前。同一个对象 ID 在日志删除完成前保持保留，不允许新准备复用该名字。目录内容、权限、文件类型、链接数及持久引用均校验，发现陌生成员、引用冲突或无效日志则停止回收并报告，不推断删除。测试先在旧二进制确认新归属回归因无日志失败，再实现与验证。

### A.5.1 实际对象状态与回收规则

新增 src/storage/PreparationJournal.inc，作为 DeltaStorage::Impl 的内部实现被包含。运行时使用现有 SessionIoLease/activePins；进程内 map 只用于同会话重试复用和对象 ID 预留，跨重启判断以已同步的日志和 SQLite 为准。UploadStorage.cpp 引入仓库已附带的 nlohmann JSON，CMake 仅补 server_core 的私有 include 路径，没有新增外部依赖。

日志格式版本为 1，路径是 .preparations/<session-id>，五个字段为 format、session、version、state、request。request 使用现有 BeginDelta 编解码，保留逻辑路径、文件大小、块大小、摘要、纳秒时间戳和基线选择器的整数精度。session/version 都严格校验为 32 位小写十六进制 ID。记录大小有界，文件为私有普通文件、单链接；路径由根目录 FD 限制，不能用日志内容指定任意文件路径。

| 日志/SQLite/文件状态 | 含义与行为 |
| --- | --- |
| 只有 .tmp 日志，正式记录尚未安装 | 尚不允许创建对象。启动只清除受控临时日志，不把其中内容当作对象删除依据。 |
| preparing，phase=2，versions 无行 | 归属已先持久化，允许对象目录或 pending/data 处于部分准备状态。活动租约内禁止回收；同会话重试复用该 version ID。 |
| ready，phase=2，versions 无行 | 原对象文件与目录同步步骤已完成，但准备 SQL 未成功或尚未执行。日志独立于该事务，不随回滚消失。重试再次核对摘要；重启选择回收对象并保留上传暂存/accepted。 |
| ready，phase=4，versions 与同会话 version_id 匹配 | 已登记待发布的版本，保留对象和日志，复用原 COMMITTING 恢复。 |
| phase=5，versions 与请求一致 | 对象继续作为不可变历史保留，只清除归属日志，原 result/current/history/snapshot 不受影响。 |
| reclaiming，无持久引用，无活动 pin | 已持久记录回收意图。允许清理已知 pending/data，文件夹不存在也可重试；先同步对象目录和 versions，再删日志并同步。日志消失且目录不存在是回收完成状态。 |
| 失效/删除的会话，有有效日志且无引用 | 可回收所属对象，但不能由“不存在 session”单独推断任意对象都是垃圾。有效归属日志和全引用检查仍是必要条件。 |
| 未知格式、引用冲突、重复 version 归属或陌生成员 | 停止回收并失败退出/报错，保留对象等待排查。 |
| 旧 .quarantine 对象没有日志 | 保留。不会凭内容相同或“当前没有 versions 行”追认归属并删除。 |

回收检查 versions、current_files、version_history、snapshot_entries、legacy_versions、transfers 和 delta_sessions.version_id。SQLite 查询失败会报错，不会被当作“无引用”。在启动串行恢复或原两把共享锁保护下做最终引用判断；运行期过期回收还必须 activePins=0。只扫描该已证明归属的版本目录，先完整校验成员只能是私有单链接普通文件 pending/data，再开始删除。陌生成员存在时连已知 data 也保留。没有递归删除或跨根目录操作。

reservePreparation 在短锁内预留唯一 ID，日志写入和对象正文 I/O 均在锁外；readyPreparation 在原对象同步成功后独立更新日志。即使日志写入失败，同会话进程内也只有一个候选预留，下一次重试继续写同一记录。回收完成前该 ID 不从预留表移除；只有对象删除与日志删除均完成同步后才释放。新一轮准备选择新 ID，旧任务没有后续按名字删除的动作，避免内部 ABA。单进程备份根的既有排他锁及同会话租约继续生效。

### A.5.1 崩溃窗口

| 注入点 | 恢复结果 |
| --- | --- |
| 初始日志文件 fsync 前 | versions 中没有新对象；可能只有受控 .tmp，启动清理后会话仍可提交。 |
| 正式日志及目录已同步，对象尚未创建 | preparing 记录足以证明归属；空回收重复执行安全。 |
| 对象目录已创建，SQLite 尚未登记版本 | 日志早已持久化。启动回收空目录，不产生无归属对象。 |
| 对象写入后、fsync 前，或对象 fsync 失败 | preparing 保留，current/history 未更新；启动回收 pending，上传数据仍可续传。 |
| 对象完成同步，准备 SQL 回滚 | ready 保留，同会话不再生成多个 ID。重启只回收该有归属对象，SQLite accepted 保持。 |
| 准备事务成功，最终 publish 尚未完成 | phase=4 与 versions 证明持久引用，归属恢复不删除，随后原 publish 完成。 |
| 最终事务完成，日志清除前崩溃 | phase=5 和 versions 保护对象，只清日志；已提交操作保持幂等。 |
| 回收删除目录后、删除日志前崩溃 | reclaiming 仍在；下次看到目录不存在，继续同步及删除日志，不生成或误删同名新对象。 |

“对象已创建但归属尚未登记”的旧顺序被消除。新流程只有对象已创建而 SQLite versions 尚未登记的窗口，此时独立日志已经存在。旧版本产生的无日志对象仍作为未知数据保留，不将重排顺序当作已经为它们补齐归属。

### A.5.1 原问题复现与修复后的实测

改动前的三对象证据保存在 /tmp/chunkharbor-r32-owner-f1bC4Bgx/baseline-repro.log。三个文件系统目录与重启后的隔离目录逐一对应如下，SQLite 中均无 versions 行。

| 对象 ID | 复现时状态 | 重启后状态 |
| --- | --- | --- |
| 55025376dc1516e43534f4d605de5fcf | .versions 下 data 为 8 字节，phase=2/version_id=NULL | .quarantine/version-55025376dc1516e43534f4d605de5fcf |
| 83fd4bbaca72e0fff437d201d09da723 | 同一内容、同一会话，versions 行为 0 | .quarantine/version-83fd4bbaca72e0fff437d201d09da723 |
| 710ca3342a41a807a84603975955672c | 同一内容、同一会话，versions 行为 0 | .quarantine/version-710ca3342a41a807a84603975955672c |

修复后专项一次实测会话为 55580a672e16a338c7524093424f2537，三次回滚始终复用 e4f72ad74066b36e43e99d2e03ebbf28，均为 ready、SQLite (2,NULL)、payload 8 字节。原来“三次失败新增三个无归属对象”的路径已变为“同会话最多一份有归属准备”。重启后对象和日志均被回收，没有进入 quarantine；上传位图仍为 00，原会话随后提交正确的新版本。

A.5 的 orphan_audit 改为断言这项已修复行为；legacy_audit 仍保留开放阻塞断言。A.4 中准备事务回滚/复制失败的恢复断言改为验证有归属对象被回收，而非要求它们进入 quarantine，其他文件、SQLite 和历史一致性断言保留。

### A.5.1 测试与实际结果

新 preparation_ownership_test.py 最终 **20 场景通过**。包括三次回滚逐次归属检查、重复失败/重启循环、日志持久化前后的崩溃、对象创建/写入后崩溃、对象同步失败、清理失败重试、过期竞争、重复提交竞争、COMMITTING 保留、持久引用保护、旧未知对象保留、ABA、新陌生成员、日志格式错误、路径逃逸及回收目录删除后的 SIGKILL。每项检查实际文件系统和 SQLite，而非只检查错误码。

在日志 fsync 屏障期间，独立 BEGIN/QUERY 可推进，证明新归属持久化没有把正文工作重新包回全局锁。异常日志和陌生成员测试验证自动回收停止且数据保留，修复测试注入后再恢复。连续重启三次结果一致。重复失败/重启循环验证每次只保留一个候选，恢复后准备目录/日志归零，已提交旧版本内容不变。应对未验证对象的保留策略没有被削弱。

Ubuntu 22.04/GCC 11.4，隔离根 /tmp/chunkharbor-r32-owner-f1bC4Bgx，复用既有私有 OpenSSL/SQLite include/library 路径，配置及 cmake --build build -j 3 成功。首次编译发现 server_core 缺少现有 vendored JSON 的私有 include 路径，补齐后构建通过。实际运行以下命令。

```bash
ctest --test-dir build -R preparation_ownership -V
ctest --test-dir build -R 'preparation_ownership|delta_commit_lock|delta_recovery|delta_lifecycle' --output-on-failure
ctest --test-dir build --output-on-failure
ctest --test-dir build -R 'delta_commit_lock|preparation_ownership|delta_recovery|delta_fault' --output-on-failure
```

完整 CTest **63/63**，51.23 s。随后增加回收中崩溃与错误日志边界断言，最终专项 **20 场景**，单跑 2.71 s，最新 A.4/归属/崩溃/故障组合 **4/4**，6.33 s，其中归属 2.41 s。日志为 baseline-repro.log、ownership-red.log、config.log、green-build.log、ownership-green.log、ownership-focused.log、full-test.log、final-build.log、ownership-final.log、ownership-regression.log。旧二进制运行新回归时因缺少 .preparations 日志而失败，红灯与原三对象复现均发生在生产改造前。测试搭建时修正了 COMMITTING 恢复用例中未移除的故障触发器，之后通过，未弱化产品校验。

Windows 11/Qt 6.8.3/MinGW 13.1，cmake -S . -B build-windows-gui 和 cmake --build build-windows-gui -j 2 成功；QT_QPA_PLATFORM=offscreen 下执行 ctest --test-dir build-windows-gui -LE native-desktop --output-on-failure -j 2，**33/33**，119.57 s。日志为 build-windows-gui/owner-config.log、owner-build.log、owner-test.log；缺可选 Vulkan 头提示仍不阻断构建。没有修改 Qt GUI。真实 SSH 业务端到端、原生桌面专项、MSVC/TSan、真实掉电和真实盘满、GB 文件传输均未执行。通过 SSH 在 Ubuntu 执行隔离测试不等于 GUI 数据链路验收。

### A.5.1 验收与剩余边界

**A.5.1 在当前可信单服务/64 MiB 范围满足本轮验收条件。新的三次回滚增长路径已解决，新有归属失败对象具备最终回收路径。**

本轮不修改 schema，旧 v2/v3/v4 数据库与未完成会话走原迁移/恢复流程，新目录为空时兼容旧备份根，完整回归包含这些夹具。内部准备日志格式是本轮新增持久化格式，未知格式失败关闭；不支持把“旧程序不知道日志”当成安全回退方案，活动日志下回退旧服务程序未验证。

仍需保留以下风险边界。

- 以前已经产生、无法证明归属的隔离对象继续占用空间，本轮不会自动追认或删除。旧复现中的三个对象属于这个类别。
- 新的未提交准备按会话复用、受既有活动会话数量和 TTL 管理；已提交历史/快照按原策略保留，长期版本增长属于 R4，文件及复制峰值空间预算仍需 B 阶段处理。
- 持续 I/O 或日志损坏可能阻止回收/启动。系统保留日志与对象并报错，故障消除后可重试；不能在物理删除持续失败时承诺空间已经释放。
- 归属恢复选择回收未登记准备对象，保留已确认上传数据。重新提交会重新物化对象，可能增加本地磁盘读取成本，本轮不作性能优化承诺。
- legacy 在线索引锁隔离仍未实施，留 A.5.2；整个 A 阶段继续未关闭，文件上限和配额保持 64 MiB/512 MiB。固定分块、协议、Qt GUI 和历史对象保持原有行为，未开展 R4 GC。
- 没有新增后台回收线程。串行启动或现有过期清理持有必要锁执行元数据回收，活动准备由原租约保护，必要的日志/目录同步仍有磁盘时延风险。

修改文件为 PreparationJournal.inc、DeltaStorage.inc、UploadStorage.cpp 的 JSON include、CMakeLists.txt、故障注入库、归属专项、A.4/A.5 回归断言，以及本页和 PROGRESS。既有工作区改动保留，未 commit/push，本轮停止于 A.5.1。

## A.5.2：Legacy 在线索引 I/O 锁隔离（2026-11-06）

### 审计、基线与实施顺序

本轮仅处理 A.5 已复现的 legacy 慢读阻塞。开工 HEAD 为 aa22a6f，工作树已有 A.1–A.5.1、SSH/GUI 等未提交改动；未重置、提交或推送。先用既有二进制与屏障保存复现，再运行新测试确认红灯，然后修改 legacy 路径并执行专项、跨平台及一致性回归。

| 审计问题 | 当前源码依据与结论 |
| --- | --- |
| 谁触发索引 | 服务端 DeltaStorage::Impl 构造恢复调用 legacy()；在线 UploadHandler 接收 BEGIN_DELTA（24），投递 DiskExecutor，再调用 DeltaStorage::begin。Linux CLI backup/preview、GUI 增量/自动备份及其他 fixed_block_delta_v1 客户端会走 BEGIN。旧 CLI upload 使用 handshake/upload，提交留下 COMMITTED transfer，供以后补索引。 |
| 读取什么 | 改造前先打开 files/<logical_path> 当前副本，hashFd 校验，再 object() 校验/创建 .versions/<legacy-session>/data；写事务里又读当前副本计算块摘要。不读 .delta 暂存。改造后只读取已提交会话对应的不可变版本对象。 |
| 为什么持锁 | 原 begin 整体持 u.mutex 和 u.dbMutex，legacy 沿用调用者锁，保证登记、current 和会话状态协调；该作用域也意外包含所有正文 I/O。启动时尚无工作线程，不存在在线竞争。 |
| 原锁内慢操作 | target() 目录遍历/目录同步、当前文件整文件哈希、object 已存在对象的双侧哈希或复制/文件及目录 fsync，以及 BEGIN IMMEDIATE 内的逐块 pread/SHA-256；块数增长也增加 SQL 工作。 |
| 谁能改动对象 | files 当前副本可以被提交的 rename 替换；暂存可写，不能用作不加保护的索引来源。服务内部不原地修改已提交 .versions 对象。旧提交在记录 COMMITTED 前完成 committedFile 的复制及同步；旧根启动恢复也先补齐并验证对象。外部直接写备份根不受 C++ 锁控制，仍需身份与完整性复核。 |
| 可复用的保护 | UniqueFd、安全 openat2、不可变对象及 COMMITTED transfers 持久引用。PreparationJournal::preparationReferenced 已检查 transfers，因此这些对象不会作为无引用准备被回收。Delta 会话 activePins/sessionIo 不能无条件套用到旧会话；本轮增加独立的 RAII 索引所有者标记。 |
| legacy 是否仍使用 | 仍是受支持的 upload 协议/CLI 路径，现有协议、历史下载及迁移测试实际使用；本轮真实 SSH 链路也先旧上传再增量更新。未删除能力或旧数据兼容逻辑。 |

完整相关调用链：旧 UploadClient/协议客户端 → BEGIN_UPLOAD/DATA_CHUNK/COMMIT → UploadStorage::commit → committedFile 持久化不可变对象 → transfers COMMITTED 与历史记录；随后 DeltaExchange/备份客户端 → BEGIN_DELTA → UploadHandler → 有界 DiskExecutor → DeltaStorage::begin → legacy(请求路径) → DeltaReady → 原有 manifest/plan/query/commit。历史下载可直接通过旧 transfer/history 找到不可变对象，无需等待补索引。

### 原阻塞的确定性证据

隔离测试根由 TemporaryDirectory 创建。旧二进制取自 A.5.1 验证目录 /tmp/chunkharbor-r32-owner-f1bC4Bgx/build，没有覆盖正式服务。新测试保留 --baseline 模式，仍可复现旧行为；旧 lifecycle 的审计输出也另存。

1. 通过旧协议真实上传并提交 legacy，确认对象及 transfer 已存在；建立 A/B 两连接。
2. 用已有 CH_PLAN_TEST_GATE 按 legacy 对象 ID 武装 pread 屏障；A 对该路径请求增量预览，等待 .entered，而非用随机延时猜测是否开始读取。
3. B 对独立路径发 BEGIN。旧行为约 **0.3029 s 超时**；/proc 线程等待状态为主线程 ep_poll、读线程 hrtimer_nanosleep、另一工作线程 futex_wait_queue。
4. 屏障期间独立 SQLite 连接可执行 BEGIN IMMEDIATE/ROLLBACK，排除正在占用 SQLite 写事务；只有一个慢读任务，另一个工作线程已开始处理 B。结合旧 begin 在第一行获取两把锁的源码，阻塞定位为共享存储锁组，而非 DiskExecutor 满或 SQLite 写事务。
5. 新正常模式跑旧二进制，在“legacy body read still blocks independent BEGIN”断言红灯。修复后相同屏障未释放时 B 正确返回 base_version=null；专项记录约 **2.1–8.9 ms**，测试期限 1 s。这是本机确定性并发证据，不是吞吐或生产时延承诺。

### 实际改造与锁顺序

修改前在线锁范围：u.mutex/u.dbMutex → expire → 全量 legacy（读/哈希/复制/同步/索引事务）→ baseline/配额与创建 Delta 会话 → 解锁。

修改后：

| 阶段 | 锁与事务 | 工作及所有权 |
| --- | --- | --- |
| 候选快照 | scoped_lock(u.mutex,u.dbMutex)，无显式写事务 | 查请求路径的未登记 COMMITTED transfer；启动 legacy() 仍全量处理。已登记项不重建。 |
| 固定输入 | 同一锁组，无显式写事务 | 重查是否已登记、取得 LegacyIndexLease 独占标记；安全打开不可变 FD，保存 dev/ino/size/mtime/ctime 与 current 快照。 |
| 计算 | 两把锁均释放，无 SQLite statement/事务 | 单次流式遍历，计算整文件 SHA-256 和每块摘要。正文缓冲至多一个既有允许的块，摘要向量受既有 65,536 块限制；不整体读取文件，不创建/覆盖版本对象。 |
| 复核与安装 | 同一锁组；复核完成后 BEGIN IMMEDIATE 到 COMMIT/ROLLBACK | fstat 固定 FD，安全重开原命名对象并比较身份；校验大小、时间、单链接、完整摘要、transfer 身份和 COMMITTED 状态，以及 current 未变。事务登记 versions、version_chunks、legacy_versions，并保持 INSERT OR IGNORE current 的既有语义。事务中没有正文读取、哈希或文件复制。 |
| 返回 BEGIN | 同一锁组，原有会话登记语义 | legacyPending 再检查快照与重新取锁间新完成的旧上传，存在则 BUSY，防止将尚未索引的文件当作 Absent；随后原 Exact/Absent、路径占用、过期/配额与创建会话检查。 |
| 释放 | 先销毁 statement/FD 并退出锁作用域，再 RAII 取得同一锁组 | 无论成功、I/O 异常或 SQL 回滚，释放索引标记。同一旧会话的并发 BEGIN 返回已有 BUSY；其他路径不补索引该对象，可独立推进。 |

没有引入会话互斥锁、条件变量、后台线程或新的锁获取顺序。所有锁组沿用 std::scoped_lock；租约析构不在已有锁作用域内重入。工作及回调继续归现有 DiskExecutor/loop，存储实例在停止并加入工作线程后销毁。

按需补索引只移除“无关 BEGIN 顺带索引所有旧文件”的内部副作用；请求路径的版本选择、清单及固定分块结果不变。启动仍补齐所有旧 transfer，然后才执行未知对象隔离。正常旧对象丢失或损坏会报错，不能退回当前路径猜测历史内容；重启保留 UploadStorage 原有恢复机制。未改协议、schema v4 或 A.5.1 日志格式。

### 身份、生命周期与一致性

FD 固定 inode 的引用，不单独承担内容稳定性保证。内部不可变约定、全文件摘要和前后 stat 联合验证；安装前通过原 openSafe/regularFile 重开，继续拒绝符号链接、跨卷逃逸、不安全权限和多硬链接。对象路径被改名/替换时，即使旧 FD 仍可读，也返回 CONFLICT，不把无效命名对象登记为版本。当前文件副本被安全重命名则不改变所固定的历史对象；索引仍对应旧版本 ID 和真实原文。

索引期间不申请 Delta 会话租约：被读取对象已经由 COMMITTED transfer 持久引用，并且当前实现没有 R4 历史版本 GC。A.5.1 清理查询该引用，不能删除它；启动扫描与在线 worker 不并行。LegacyIndexLease 只排除重复索引，不伪装成跨重启的对象归属。进程崩溃丢失内存标记后，COMMITTED transfer 和已同步对象仍在，启动重新计算并事务登记。

若 A.4 并发提交切换 current，复核检测变化，旧索引尝试返回 CONFLICT。重试以新 current 为快照登记旧对象，INSERT OR IGNORE 不覆盖新指针。Exact/Absent 在 BEGIN 返回前仍检查；后续提交仍沿用原 checkBase/publish 的再次复核。SQL 失败回滚 versions/chunks/mapping/current，不影响已提交 transfer、历史记录或对象。客户端断连不撤销已安装索引；任务有限完成并释放 FD/标记，结果回调按既有弱引用处理。关闭等待 worker，SIGKILL 后原子 SQL 与启动重试决定已安装或重新补索引。

### 专项与回归结果

新增 legacy_index_lock_integration，**23 场景通过**（最终补强总耗时断言后 8.53 s），分组如下：

| 场景 | 实际断言 |
| --- | --- |
| normal/empty/partial_block/max_file/max_blocks | 正常、空文件、末尾短块、真实旧协议 64 MiB 上传，以及 65,536 块索引边界；核对每块 SHA/长度、整文件原文、预览零缺块和历史记录。最大块数使用独立夹具事务生成正确的 terminal transfer/chunk ledger，实际执行 65,536 块索引及 manifest，不发送 65,536 次旧协议网络 ACK。 |
| progress/duplicate | 确认 pread 已进入后，独立 BEGIN 和 SQLite 写事务在 1 s 内完成；重复同对象索引返回 BUSY，无第二次进入屏障。 |
| same_path_update/exact_conflict | 对真实提交版本构造待补索引夹具，预先准备的 A.4 更新在屏障期间完成发布；旧尝试 CONFLICT，重试指向新版本，旧对象保留。错误 Exact 也拒绝。 |
| rename_current/rename_object | 当前副本更名不影响不可变旧内容；对象更名后 /proc FD 仍引用原对象，但安装拒绝；恢复命名后重试成功。 |
| missing/read_failure/checksum_failure/mutate_during_read/symlink/sql_rollback | 缺失、EIO、损坏、读取期间修改、符号链接和索引事务失败均拒绝；检查 SQLite 无半份登记、transfer 仍 COMMITTED、对象/历史保留，解除故障后重试。错误码沿用 INTERNAL_ERROR/INVALID_PATH，身份竞争使用 CONFLICT。 |
| disconnect/shutdown/restart | 客户端断连、正常关闭加入 worker、屏障中 SIGKILL 后启动恢复；再次 BEGIN 获得正确旧版本，历史不重复。 |
| lease_operations/owner_cleanup | 屏障期间独立路径依次完成 A.1 清单、A.2 物化、A.3 查询、A.4 提交；另一场景让有归属准备失败并过期，独立 BEGIN 实际回收其日志/对象，旧对象不被删除。 |
| cycles | 连续 10 次旧上传/索引/预览，逐次检查数据与历史；关闭连接后 /proc FD 数回到初始值。异常场景还验证释放标记后的重试，不只检查返回码。 |

曾在测试搭建中修正阶段名 TRANSFERRING 和安全路径错误码 INVALID_PATH 两项断言，未修改产品协议/安全检查。所有数据均在独立临时根，正式服务和真实备份根未操作。

Ubuntu 22.04/GCC 11.4 隔离目录 **/tmp/chunkharbor-r32-legacy-duQnBGZX**，执行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DCHUNKHARBOR_BUILD_GUI=OFF -DCHUNKHARBOR_BUILD_TESTS=ON \
  -DOPENSSL_INCLUDE_DIR=/home/ning/ChunkHarbor/build/deps/openssl/usr/include \
  -DOPENSSL_CRYPTO_LIBRARY=/usr/lib/x86_64-linux-gnu/libcrypto.so.3 \
  -DSQLite3_INCLUDE_DIR=/home/ning/ChunkHarbor/build/deps/sqlite/usr/include \
  -DSQLite3_LIBRARY=/usr/lib/x86_64-linux-gnu/libsqlite3.so.0 \
  -DCMAKE_CXX_FLAGS=-I/home/ning/ChunkHarbor/build/deps/openssl/usr/include/x86_64-linux-gnu
cmake --build build -j 3
ctest --test-dir build -R legacy_index_lock -V
ctest --test-dir build --output-on-failure
ctest --test-dir build -R 'legacy_index_lock|delta_plan_lock|delta_materialize_lock|delta_query_lock|delta_commit_lock|delta_lifecycle|preparation_ownership|delta_recovery|delta_fault|version_history|directory_snapshot' --output-on-failure
```

配置/构建成功，完整 CTest **64/64**，63.47 s；最新专项及 A.1–A.5.1/历史/快照/崩溃组合 **13/13**，24.86 s。新读屏障复用原故障库，本轮未修改故障库实现。日志有 baseline-lifecycle.log、baseline-measured.log、legacy-red.log、config.log、build.log、build-final.log、legacy-focused-final.log、full-test.log、focused-regression.log、legacy-final-proof.log；最后一份为加入独立请求总耗时小于 1 s 的显式断言后的 23 场景复验。

Windows 11/Qt 6.8.3/MinGW 13.1 执行 cmake -S . -B build-windows-gui、cmake --build build-windows-gui -j 2；QT_QPA_PLATFORM=offscreen 下 ctest --test-dir build-windows-gui -LE native-desktop --output-on-failure -j 2，配置/构建成功，**33/33**，127.42 s。日志 build-windows-gui/legacy-config.log、legacy-build.log、legacy-test.log。这是跨平台共享层/Qt 回归，Linux 专项实际在 Ubuntu 运行，不将 Windows 结果当成 Windows 服务端验收。

补充真实 SSH 数据链路：Windows Python 协议探针 → Windows OpenSSH 自动本地转发 → 隔离 Ubuntu 测试服务。先旧上传 aaaabbbb，再对同路径增量为 zzzzbbbb，只上传缺失 4 字节；确认新 current、两个历史记录、旧/新不可变对象和 SQLite integrity。独立根 /tmp/chunkharbor-r32-ssh-u8oxvhc3/backup，测试服务及转发均已结束；日志 build-windows-gui/legacy-ssh.log，探针脚本 legacy-ssh-probe.py。不是 Qt GUI 端到端或吞吐测试。未执行原生桌面、MSVC/TSan、真实掉电/坏盘/盘满、GB 文件或多文件并行测试。

### 验收与下一步边界

独立只读代码复核未发现本轮引入的严重或重要问题；该复核是静态检查，不作为额外测试结果。A.5.1 PreparationJournal.inc 与已验证基线的 SHA-256 相同，本轮未改动。

**A.5.2 在本轮范围内满足验收条件，已复现的 legacy 正文慢读占用共享锁阻塞已消除。** 修改仅为 DeltaStorage.inc、CMake 专项注册、新增 legacy_index_lock_test.py、lifecycle 的 legacy 断言，以及本页/PROGRESS；既有其他工作树内容保留。

仍需 A.5.3 单独综合验收：全量启动扫描/校验、恢复与隔离的串行成本；过期清理中的文件/目录同步；普通元数据扫描和最多 65,536 条索引写入的原子事务/SQLite 同步持锁成本；两工作线程同时繁忙时的有界排队、停止加入和回调生命周期；A.1–A.5.2 的组合及失败收敛。索引写事务仍必须原子提交，未为降低等待而拆掉一致性边界。安全 open/stat 等必要元数据操作也仍可能受物理磁盘延迟影响，不能声称所有共享锁等待都消失。

旧未知隔离对象、日志持续损坏或 I/O 失败，以及已提交历史/快照的长期空间增长继续沿原策略处理，未实施 R4 GC。不可变对象读取的安全证明依赖现有单服务 flock、受控根和持久引用；未来引入历史 GC 时必须将该读取纳入版本读引用协议，不能照搬“当前没有 GC”的前提。

文件上限 **64 MiB**、在途配额 **512 MiB** 保持不变，协议/schema/Qt GUI/固定分块/A.5.1 回收语义未改。**整个 R3.2 A 阶段仍未关闭，下一轮仅建议 A.5.3 综合验收，本轮不进入 B，未 commit/push，停止于 A.5.2。**

## A.5.3：A 阶段最终综合验收与关闭评估（2026-11-06）

### 本轮范围、发现与结论

**R3.2 A 阶段通过当前可信单服务、64 MiB/65,536 块设计边界内的验收，正式关闭；可以安排 R3.2 B，但本轮不实施 B。** 这是整文件/多块长操作的锁隔离与一致性验收，不是 1 GiB 支持、所有系统调用无等待或长期性能验收。

本轮先复用既有测试及源码审计，再补充元数据计时、工作池占满和预览记录上限夹具。实际发现并最小修复了一条此前未覆盖的阻塞：20,000 个合法过期预览由一个在线 BEGIN 全量清理，约 23.033 s 期间独立请求超时。随后对照测试又定位到未过期预览的全量 JSON 解码成本，修复后才重新完成整轮回归。没有用先前 A.5.2 的通过结果替代本轮验证。

开工仍为 HEAD aa22a6f 的未提交工作树，含已验证的 A.1–A.5.2 和用户既有 SSH/GUI 改动；均保留。未 commit/push。业务修改仅位于 DeltaStorage.inc 的过期候选/在线清理预算及配额统计，未动协议、schema、Qt GUI、固定分块、64 MiB 文件上限或 512 MiB 在途配额。

### A.1–A.5.2 机制及本轮复验

| 子任务 | 已有机制 | 本轮实际复验入口 |
| --- | --- | --- |
| A.1 finishManifest | 锁内固定不可变基线 FD/清单快照，锁外差量比较，返回锁内安装计划；计数 pin 保护重复规划 | delta_plan_lock：8 场景 |
| A.2 planPage | 独占 sessionIo + activePins；锁外物化复用块和同步，复核后确认复用状态 | delta_materialize_lock：10 场景 |
| A.3 query | 固定 staging FD 和已确认块快照，锁外重验；返回前复核，损坏块撤销确认 | delta_query_lock：12 场景 |
| A.4 commit/checkBase/publish | 会话独占及生命周期租约；锁外整文件/基线哈希、对象及发布副本复制/同步；复核基线和 inode 后更新指针 | delta_commit_lock：26 场景，含回滚、指针/身份竞争、应答丢失和崩溃 |
| A.5 生命周期 | 活动 pin 延迟过期，phase=7 可重试清理，幂等结果/历史保留，FD 和路径预留复核 | delta_lifecycle：12 场景 |
| A.5.1 持久归属 | 独立 .preparations 日志先同步再创建对象；引用检查、reclaiming 与可重试回收 | preparation_ownership：20 场景 |
| A.5.2 legacy | 按路径固定不可变对象，锁外整文件及块哈希，锁内身份/transfer/current 复核和事务登记 | legacy_index_lock：23 场景 |

这些场景存在交叉，不把数量相加宣称独立覆盖率。完整 CTest 另外复验 R1/R2、旧 v2/v3 夹具、路径安全、上传/续传/增量、当前/历史下载、目录快照及恢复。Windows gui_worker_test 实际覆盖 auto 新文件、有效续传、损坏/过期检查点、取消等；目录恢复及单文件下载也在现有非原生 Qt 测试中运行。

### SQLite、共享锁与事务边界

此处“共享存储锁”是多业务共用的 u.mutex/u.dbMutex，两者都是互斥锁，不是 std::shared_mutex 的读锁。Delta 沿用 scoped_lock 同时获取锁组；旧上传从 session mutex 进入短 dbMutex 阶段，查询先退出 retained 的 u.mutex 作用域再取得 session mutex，没有发现持 dbMutex 再等待 session mutex 的反向闭环。

| 路径 | 共享锁/事务范围 | 正文 I/O 与必须保留的串行部分 |
| --- | --- | --- |
| legacy 索引 | 快照/复核锁组；BEGIN IMMEDIATE 至登记 versions/chunks/legacy/current 后 COMMIT | pread 和两类 SHA 在锁外。事务仍逐块写 SQL 元数据，SQLite 页/WAL/同步 I/O 必然存在，不把它误称为文件正文逐块读取。 |
| current 查询/列表/历史/快照 | dbMutex；当前更新与历史 trigger 在发布事务；快照捕获全清单用单事务 | 查询主要是有界页；快照上限 10,000 条。原子捕获和发布不可任意拆开。没有为追求吞吐移除 Exact/Absent 或 history/current 约束。 |
| 清单/计划/确认状态 | 必要快照锁组；安装整份计划、撤销坏 accepted 或准备版本时事务提交 | 基线读取/复用物化/续传重验在锁外。块清单及状态变更仍原子安装。 |
| commit / COMMITTING / publish | 准备事务登记版本和 phase=4；发布事务更新 current/history/result/phase=5 | 对象数据与目录持久化先于相关指针提交，整文件读取、复制、数据同步在锁外；命名空间复核/rename 及 SQL 提交仍串行。 |
| DeltaStorage::write | 一个协议块的验证、pwrite/fdatasync、accepted 更新仍在锁组内 | **未隔离所有单块 hash/fsync。** 当前临界段最多一个既有允许的块（1 MiB 数据帧减 60 B），用于原有持久 ACK、同会话块状态和清理互斥；不随整文件大小增长，也不一次覆盖整份上传。慢盘仍能延长它，本轮不据此承诺全部请求时延。 |
| 在线 expire/reclaim/forget | 单对象失效、引用验证、受控 unlink/目录及日志同步、辅助行删除保留原锁组与事务 | 无正文扫描。原子归属/路径释放及失败重试顺序需要串行；本轮限制每个 BEGIN 的对象数量，消除全批次放大。 |
| 启动恢复 | 就绪前单线程恢复；必要 publish/清理仍复用同一原子机制 | 尚无在线 worker，不存在与在线清理争锁；耗时可能随待恢复对象和已确认数据增长。 |

SQLite 仍为单连接、WAL、synchronous=FULL、foreign_keys=ON，没有新增无限 busy 重试。区分应用互斥等待、SQLite 文件锁/同步耗时、物理磁盘争用与工作池排队。测试期间独立 SQLite BEGIN IMMEDIATE 能推进只能证明当时没有 SQLite 写事务占用，不能证明所有未来磁盘时延有界。

测试库增加可选 CH_A53_SQL_COST，测量同一线程 BEGIN IMMEDIATE 进入至 COMMIT 返回，按 legacy_versions 登记识别索引事务；只记录结束类型、SQLite 返回值和时间，不记录 SQL/路径/文件原文。正式程序未链接该库。

最新 accepted-combined.log 的样本：

| 旧索引块数 | BEGIN 请求总耗时 | SQLite 索引事务（含提交同步） |
| --- | --- | --- |
| 0 | 3.255 ms | 1.301 ms |
| 4,096 | 57.669 ms | 22.896 ms |
| 65,536 | 935.674 ms | 394.420 ms |

其他本轮重复样本最大块数事务约 276–394 ms。索引登记确实有与块数成正比的串行成本，但上限仍为 65,536，块数不会随 B 的文件上限直接扩大；事务中没有正文 I/O。当前没有证据需要拆分事务或更换 SQLite 架构。这是隔离 VM 的样本，不是 p95、真实慢盘上界或正式吞吐结果。

### 本轮确定性阻塞与最小修复

1. 用真实空文件预览生成合法 PLAN_READY 模板，在独立根事务复制至现有 20,000 预览上限，元数据预算仍在既有 64 MiB 内。全部 created=0 后，观察实际删除已经开始，再发独立请求；旧 expire 整批持续持锁，BEGIN 约 **23.033 s**，独立请求超时。preview-expiry-audit.log 和 preview-backlog-red.log 保存证据。
2. expire 增加内部预算，在线 begin 传 32；默认启动调用仍不设该批次预算，完整恢复后才就绪。ORDER BY mode,created,id 优先处理占用上传配额/路径的失效上传；activePins 继续排除活动任务。扫描 statement 在删除前销毁。每个对象的 reclaiming、引用检查、文件与目录同步、事务和失败重试未改，不是取消或永久禁用清理。
3. 32 预算后，全部预览 JSON 仍在普通配额统计中解码，独立查询约 1.18 s；quota-scan-red.log 的 1 s 截止断言失败。begin 改用现有 mode/phase 列 GROUP BY 计数，仅为有界活跃上传解析 size，不引入 JSON1 或 schema 依赖。
4. 追加**未过期** 20,000 条对照后，发现 expire 仍逐条解码未过期请求；测试库的 SQL-step 标记确认已经进入持锁扫描，fresh-preview-red.log 再次红灯。改为先用现有 mode/created 在 SQL 筛选过期候选，再做原请求 TTL 与 activePins 复核。created < now - TTL 与原 now - created > TTL 严格等价，未延长 TTL。
5. 最新对照：20,000 条未过期记录全部保留，独立 query **4.306 ms**；过期后在线仅删除 **32** 条，剩 **19,968**，独立 query **31.296 ms**，触发 BEGIN 总计 **69.076 ms**。另用 64 过期预览 + 32 实际 staging 上传确认上传优先，单轮清理 32 个上传约 **83.776 ms**，64 个预览留待后续回收。
6. 在线未处理完的失效记录由后续 BEGIN 分批推进，或由启动全量清理；持续新 BEGIN 的入站数量小于每轮清理预算，不会因新增一个请求抵消整批清理。活动 pin 与持续 I/O 故障仍按原语义延后/报错，未承诺无流量时后台定时清理。没有新建线程或调度系统。

剩余 19,968 条预览在首次启动全量清理约 **26.782 s**，随后连续两次启动约 **6.373 / 6.135 ms**，预览全部消失，版本对象、活跃上传和 SQLite 完整性保持正确。只为该批量**测试观察者**设有限 60 s 就绪等待；产品 idle/request 超时未改。这不是用延长客户端超时掩盖在线阻塞，在线对照截止仍为 1 s。

### 启动恢复、清理与资源验收

| 状态 | 本轮依据与结果 |
| --- | --- |
| 未完成会话与 accepted | resume、query restart/corrupt/missing、lifecycle 等真实重启测试；正常块保留，无法验证的坏块不报告已确认。 |
| preparing/ready 未登记对象 | ownership 的三次回滚、日志/对象创建与同步故障、连续重启；复用持久归属，最终可回收，上传数据/会话保留。 |
| COMMITTING / 已 rename 未提交元数据 | commit crash_prepared/crash_published、recovery/fault；按固定对象和基线复核补齐发布，不重复历史；持续故障拒绝就绪并附会话 ID，解除故障后重试。 |
| 已提交及响应丢失 | lost_response、重复 commit/query、history/snapshot；保留幂等 result、旧版本和 current；迟到失败不撤销成功发布。 |
| 已知归属无引用对象 | owner 的引用保护、清理失败重试、reclaim_crash 与 ABA；重复执行安全，日志在对象/目录同步后才最终移除。 |
| 未知旧隔离对象或无效归属日志 | unknown/journal_corrupt/journal_escape；未知对象保留隔离，不猜测删除；损坏日志导致保守失败，修复后再恢复。 |
| 活动规划/物化/query/commit/legacy | 原专项 + lifecycle/owner/legacy 组合，pin 或持久引用保护；活动结束后可过期和回收，不永久占用路径。 |
| 本轮上限夹具 | fresh/expired 20,000 对照、32 上传优先、每组连续三次重启；无对象数持续增长，SQLite integrity/foreign_key 和对象/当前数据匹配。 |

启动顺序仍是 UploadStorage 恢复 → 准备日志/失效对象/legacy 索引/未知对象隔离 → COMMITTING 发布与已提交暂存清理 → current 和 accepted 核验 → 工作池及就绪。端口可能提前 bind，但 LISTENING 和 HELLO 业务处理在恢复完成后；不能用 TCP connect 成功替代就绪。完整启动仍可能耗时，R3 B/C 需要预算较大数据的启动时间，不将串行恢复改成未受保护的并行删除。

未知旧隔离数据允许继续保留，边界是只生成可审核候选、核实归属后单独管理，不能用本轮已知对象清理权限去删除。它、保留历史/快照与长期 WAL/会话记录增长仍占磁盘，属于后续 R4 的管理范围。物理 I/O 持续失败不能保证立即释放空间。

### DiskExecutor 与关闭验证

- 实际服务配置 **2 worker / 64 outstanding**；构造器仅允许 1–64 worker、1–65,536 capacity。outstanding 包括等待、运行、已完成待回调，三者共同受同一上限约束。默认上传 32 连接，显式最多 64，单连接一个磁盘请求并 pauseReading。
- trySubmit 在 Reactor 中只做有界入队，返回 Full/Stopped；UploadHandler 返回 BUSY。任务未运行前不会取得存储租约。新增 weak_ptr 断言验证 Full 拒绝后 work/completion 捕获均释放，拒绝 work 未执行；没有在满队列上阻塞 Reactor 等空位。
- worker 捕获有限输入，完成后先释放 work captures，再存结果并写 eventfd。完成传输独立于 loop 普通 pending 队列，在 loop 上 dispatch、回调后释放 outstanding。客户端连接以 weak_ptr 验证；断连不让完成回调修改新连接/会话。
- 现有确定性 unit 以 2 worker/3 capacity：两任务被 promise 挡住，第三排队、第四 Full，Reactor 心跳释放屏障，最终三回调在 loop 线程完成且 outstanding=0；覆盖 Stopped、重复 stop、析构取消、回调异常后的清空和普通 loop 队列满。
- 新真实服务测试让两个不同路径规划都进入锁外 pread 屏障；第三磁盘请求只能排队，HELLO 在 1 s 内完成，独立 SQLite 写事务也完成。/proc 显示主线程 ep_poll、两 worker hrtimer_nanosleep，证明此处是工作线程占满，而非全局锁或 SQLite 锁互等。释放后任务及第三请求正确完成，FD 回到初始值；再过期实际删除两任务状态验证 pin 已释放。
- 关闭分支先断连，发送 SIGTERM，**保持两 worker 屏障，直到主线程进入 futex/join 等待且进程仍存活**，再释放；要求 4 s 内 exit=0，并验证两个计划 phase=1、missing 元数据正确落库。修正了“发终止信号后立即释放”不能证明已经进入关闭的测试漏洞。
- stop 只在关闭阶段等待有限任务并 drain；worker 不等待 loop completion，因此不会因 Reactor 已退出而互锁。无法承诺内核永久 D-state/硬件挂死的线程能在应用级强制 join 时限内结束；该极端风险未测试。两个长任务占满导致普通磁盘请求等待，是有界 FIFO 排队，当前未复现线程池死锁或无限队列，不构成单独引入新调度框架的依据。

### 本轮真实命令、结果与未覆盖项

Ubuntu 22.04/GCC 11.4，新独立目录 **/tmp/chunkharbor-r32-accept-Pv3cRHvQ**，首次配置：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DCHUNKHARBOR_BUILD_GUI=OFF -DCHUNKHARBOR_BUILD_TESTS=ON \
  -DOPENSSL_INCLUDE_DIR=/home/ning/ChunkHarbor/build/deps/openssl/usr/include \
  -DOPENSSL_CRYPTO_LIBRARY=/usr/lib/x86_64-linux-gnu/libcrypto.so.3 \
  -DSQLite3_INCLUDE_DIR=/home/ning/ChunkHarbor/build/deps/sqlite/usr/include \
  -DSQLite3_LIBRARY=/usr/lib/x86_64-linux-gnu/libsqlite3.so.0 \
  -DCMAKE_CXX_FLAGS=-I/home/ning/ChunkHarbor/build/deps/openssl/usr/include/x86_64-linux-gnu
cmake -S . -B build
cmake --build build -j 3
ctest --test-dir build --output-on-failure
ctest --test-dir build -R 'a_stage_acceptance|disk_executor|delta_plan_lock|delta_materialize_lock|delta_query_lock|delta_commit_lock|delta_lifecycle|preparation_ownership|legacy_index_lock|delta_recovery|delta_fault|history|directory_snapshot' -V
```

最终配置/构建成功，完整 CTest **65/65，88.13 s**；关键锁/生命周期/历史/快照/恢复/工作池组合 **17/17，54.02 s**。新增 a_stage_acceptance 的 5 组含索引三档计时、批量清理优先级、工作池占满、占满时关闭，以及 fresh/expired 预览上限与最终回收；复用旧专项，不复制其故障矩阵。新的 runtime 23 s 阻塞和 fresh 扫描测试曾实际失败，修复后才通过。SQL 计时缺失的首次红灯只表示测试测量尚未接入，不当作产品缺陷。

日志：accepted-config.log、accepted-build.log、accepted-full-test.log、accepted-combined.log 为最终证据；preview-expiry-audit.log、preview-backlog-red.log、quota-scan-red.log、fresh-preview-red.log、sql-cost-red.log 和中间 focused/build 日志保留排查过程。a53-storage.patch 是相对 A.5.2 隔离源码的本轮业务差异。PreparationJournal.inc 的 SHA-256 与 A.5.2 基线一致，归属/回收协议未改。

Windows 11/Qt 6.8.3/MinGW 13.1：本轮重新 cmake -S . -B build-windows-gui、cmake --build build-windows-gui -j 2；QT_QPA_PLATFORM=offscreen 下 ctest --test-dir build-windows-gui -LE native-desktop --output-on-failure -j 2，**33/33，121.36 s**。最终仅 Linux 存储/测试部分变动后再次配置和构建，ninja 报无工作；Qt/共享客户端目标未改变。日志 a53-final-config.log、a53-final-build.log、a53-final-test.log、a53-accepted-config.log、a53-accepted-build.log；可选 Vulkan 头缺失提示仍非构建失败。

真实联调本轮重跑，最终日志 **build-windows-gui/a53-accepted-ssh.log**：Windows Python 协议探针经 Windows OpenSSH 本地转发 → 本轮最新隔离 Ubuntu 服务。独立根 **/tmp/chunkharbor-a53-ssh-9ucmn7rt/backup**，覆盖旧上传、增量只发送变化 4 字节、断连后 query/续传、当前下载、旧历史下载、历史列表与旧快照目录恢复，并核对 SQLite/current/两个不可变对象。快照在新版本发布前创建，恢复实际在独立 Windows 临时目录写文件并核对原文。服务进程和转发均已结束，正式服务及用户备份根没有操作。这是协议数据链路联调，不能替代 Qt GUI 人工点击端到端。

未执行：真实盘满/配额卷、硬件掉电/坏盘/D-state、TSan/MSVC、Windows 原生桌面专项、Qt GUI 人工 SSH 全流程、GB 文件及长期/大规模压力与吞吐采样。注入错误、SIGKILL、源码资源上限及瞬时耗时分别列证，不冒充这些未覆盖结果。没有因极端未来测试未做而无限扩大 A。

### 六项关闭条件

| 条件 | 判定与依据 |
| --- | --- |
| 1. 长时间 I/O | **通过当前边界**：A.1–A.4/legacy 整文件或多块正文读取、哈希、复制与相关数据同步已有锁外证据；单块持久 ACK、受控命名空间和原子 SQL/元数据同步保留明确临界段。 |
| 2. 并发一致性 | **通过**：五类慢 I/O 屏障、同会话 BUSY/CONFLICT、同路径发布竞争与 pin 过期测试通过；本轮 20,000 预览阻塞已修复并有 fresh/expired 对照。 |
| 3. 崩溃恢复 | **通过**：COMMITTING、发布窗口、应答丢失、迁移、归属日志、坏块与重复启动真实回归通过。 |
| 4. 资源回收 | **通过**：新失败对象可追踪、重试复用和最终回收；分批/启动完整回收已验证。未知旧隔离对象明确保留，不算可自动删除的新孤儿。 |
| 5. 工作池有界 | **通过**：2/64 实际配置、Full/Stopped 及捕获释放、网络占满/恢复、关闭 join 和 FD/pin 释放通过；未复现死锁、无限队列或关闭卡死。 |
| 6. 回归 | **通过**：最终 Ubuntu 65/65、组合 17/17，Windows 本轮 33/33，最新代码真实 SSH 关键业务通过。 |

独立只读复核指出的关闭证明及 fresh 扫描覆盖漏洞均已核验并补齐；最终复核未发现新的正确性或锁顺序阻塞。此复核是静态证据，不增加测试次数。**A 正式关闭，允许下一轮进入 B 的设计/实现；本轮停止，不自动实施 B。**

### B 进入前检查清单（只整理，未修改）

| 检查项 | 当前源码/类型与 B 所需动作 |
| --- | --- |
| 文件入口 64 MiB | BackupWorker Windows/通用源、DirectoryScanner、Linux DeltaClient、QtDeltaClient、SourceSnapshot、DeltaExchange；GUI 文案也有 64 MiB。B 需统一增量入口及源扫描/验证，不能只改一处。旧 UploadClient/QtCliMain 保持独立兼容策略。 |
| 服务端 64 MiB / 512 MiB | UploadStorage::quotaGeometry（约 87 行）及旧 begin/启动预留检查（约 339/485 行）；Delta begin（现约 663/710 行附近）共用几何并与旧会话交叉计入 32 个/512 MiB。必须区分旧 upload 与扩展 Delta 的限制，明确新的在途容量模型后统一修改两边和恢复准入，不在本轮提高。 |
| 65,536 块/分页/帧 | ManifestPage、DeltaControl、PlanPage、SourceSnapshot/DeltaExchange/DeltaPlanner、GUI/CLI 几何校验及恢复位图。保留固定分块：1 GiB/64 KiB=16,384 块，/256 KiB=4,096 块；若保留块上限，1 GiB 最小块为 16 KiB。控制帧 64 KiB、数据帧 1 MiB、块头 60 B、每页 512 哈希、计划至多 64 页仍各自受限，不应随文件上限一起放大。 |
| 大小/偏移/进度 | BeginUpload/DeltaResult/Download 大小与 offset 为 uint64_t；块编号/数量/页为 uint32_t。SQLite INTEGER/Transfer 总量为有符号 64 位，JSON 用整数或既有十进制字符串；Qt 字节数 quint64、文件大小/seek qint64，进度最终缩放为 int 的 0–100，文件计数 int（现 10,000 上限）。Linux 当前 x86_64，B 应明确 off_t 至少 64 位编译条件；Windows DWORD 仅用于有界块读。1 GiB 无需把整个缓冲长度转成 32 位 int。 |
| 下载/恢复仍 64 MiB | Download.h MaxDownloadFileSize、Download 编解码、DownloadStorage、DownloadController、DirectorySnapshot 编解码、SqliteSnapshot capture 上限及 SnapshotRestore。当前/历史/快照必须同批兼容验证。下载仍无断点续传。 |
| SQLite 是否必迁移 | schema_v1 transfers 的 67108864 CHECK 与块索引上限属于旧 upload；Delta versions 是 INTEGER/request JSON。schema_v4 的 snapshot_entries 没有单项大小列或大小 CHECK，其文件大小限制来自 capture/协议。B 不应仅因 1 GiB 自动重建 SQLite schema；先确定旧入口保留与新能力兼容策略。 |
| 提交峰值 | 以保留历史对象实际占用 H、旧 current 为 O、新文件 S：上传 staging S + 新不可变对象 S + .publish 副本 S，在 rename 前约 **H + O + 3S**，另计 DB/WAL、其他会话/下载仍持 FD 的对象与余量。同尺寸旧版本仅一份时 H=O=S，可达 **5S**，比 R3.1 改造前约 4S 多一份发布副本。首次上传约 3S 峰值，清理后当前副本与不可变对象约 2S。不能把 512 MiB 声明配额当实际卷预算。 |
| 隔离环境/磁盘预算 | 一组 1 GiB 新/增量/历史/快照测试建议服务端独立可用至少 12 GiB、Windows 至少 6–8 GiB，并按实际保留历史 H 重新算账。当前 /tmp 所在 Ubuntu 卷约 13 GiB 可用，Windows C/D 约 57.7/28.8 GiB；这只是本轮观察，B 开始必须重新核对并预留，不能挤占正式备份或凭稀疏文件宣称容量足够。 |

推荐 B 的最小实施顺序：先确定增量/下载/快照的统一 1 GiB 能力和旧客户端拒绝/兼容策略及容量预算；再按清单集中定义与接入限制，保留旧 upload、块数和帧上限；核对 64 位算术、恢复和持久状态；最后在隔离根运行真实非稀疏 1 GiB、上限±1、增量/续传、下载/历史/快照、应答丢失和空间失败验收。先做方案及可验证差异，不简单全局替换常量，不直接转入 4–10 GiB、FastCDC、多文件并行或 R4 GC。

本轮修改文件：DeltaStorage.inc（上述最小修复）、新增 a_stage_acceptance_test.py、delta_crash_hook.cpp（可选测试计时/进入标记）、disk_executor_test.cpp（Full 捕获释放断言）、CMakeLists.txt（专项注册）、本页与 PROGRESS。没有改 PreparationJournal.inc、Qt GUI、协议或 schema。原有脏改动保留，未 commit/push，完成后停止。
