# ChunkHarbor 开发进度

- 当前阶段：M1/M2/M3已完成；M3 Windows CLI 中断、Ubuntu 服务端重启、Windows CLI 续传的用户人工验收已通过。GUI上传未实现。
- 最近更新：2026-11-01
- 本轮范围：完成M3 SQLite会话、重启核验/恢复、QUERY_SESSION、Windows CLI状态文件与续传；用户负责最后一项人工验收。历史M2结果保留在下文。

## M3 当前实现与边界（2026-11-01）

- 服务端：新会话在暂存目录同步后写 SQLite；块在哈希校验、数据/回执同步和 SQLite 块事务提交后 ACK。断连后会话按 ID 查询，`SESSION_STATUS` 用最多8192字节位图表示65536块进度，避免区间数上限阻断合法续传。
- 启动时核对暂存元数据、数据库参数及每个已确认块的实际 SHA-256；损坏块从确认集移除。`COMMITTING` 若目标已发布则重新校验整文件并协调 SQLite；已提交目标丢失或哈希不符时拒绝启动。无 DB 记录的暂存目录隔离至 `.quarantine`；有 DB 记录但暂存目录缺失标为 `FAILED`。30天未更新的 CREATED/RECEIVING 会话启动时标为 EXPIRED 并清理其受限暂存文件。
- `COMMITTING` 尚未发布且暂存块损坏时，启动核验移除坏块并退回 `RECEIVING`，避免客户端补传被状态机拒绝；相应回归用例已在隔离目录运行。
- Windows Qt CLI：`--state` 指向原子写入的本地会话文件；重新打开源文件并计算 SHA-256 后才能续传。`status --state` 查询保存进度，`status --session-id` 查询历史 ID；再次运行 `upload` 仅发送缺失块，最终核对 COMMIT_RESULT 与源文件摘要。服务端仅监听 loopback，Windows 通过 SSH 本地转发连接。
- 自动测试覆盖 Linux Debug、ASan/UBSan、Release 与 Windows MinGW/Qt；最新精确结果与人工验收输出见 `docs/M3_VALIDATION.md`。M3 源码、测试与文档在同一发布提交中同步。
- 最新验证：Ubuntu稳定隔离目录 `/home/ning/ChunkHarbor-m3-verify` Debug 30/30、ASan/UBSan 30/30、Release 30/30；Windows Qt CLI 9/9、GUI原型9/9；Windows CLI经SSH隧道真实上传、状态查询及仅补缺失块测试通过。Debug、ASan/Release完整回归均包含最新暂存清理改动。具体命令、人工步骤见 `docs/M3_VALIDATION.md`。
- 用户人工验收（2026-11-01）：32MiB随机源文件、65536字节分块（512块）；Windows CLI中断前终端显示8行ACK，Ubuntu服务端停止并用同一根目录重启后，Windows CLI查询 `MISSING_BLOCKS=503`，续传本轮发送503块并返回 `UPLOAD OK`。Windows与Ubuntu的SHA-256均为 `8db478eacc5c97f0d242e2145fe19af38a33a541ead56179380b93a20e80e3ff`。只读核对SQLite对应会话为 `COMMITTED`、`total_size=33554432`、`chunk_size=65536`、确认块数512。终端仅显示8行ACK而服务端保存9块，符合中断时最后一块已提交但客户端未打印回执的窗口。

## 发布检查（2026-10-31）

- 用户授权提交并推送 M2；README 的 Ubuntu 示例改为通用仓库路径和独立演示目录。
- 本轮 SSH `miniob-ubuntu` 连接成功，主机名 `ning-virtual-machine`；执行 `cmake --build build -j 2` 及 `ctest --test-dir build --output-on-failure`，Ubuntu Debug 构建成功，26/26 通过。

## 已完成

- [x] M3首个子任务：`src/storage/schema_v1.sql` 定义会话/确认块约束与单目标活跃会话唯一性；`docs/M3_SQLITE_DESIGN.md` 写明 ACK 事务边界及 COMMITTING 发布窗口恢复判据；独立 SQLite 测试验证重开持久性、外键、唯一约束。此项只验证 schema，不表示运行中的服务端已使用 SQLite。
- [x] M3第二子任务：`SqliteStore` 通过 RAII 打开/关闭 SQLite、要求 WAL/FULL/外键，空库执行 v1 事务迁移，再次打开验证版本，拒绝未知版本及非空无版本数据库。仅限磁盘线程调用；上传业务尚未调用。
- [x] M3事务接口基础：`SqliteStore::createTransfer`、`confirmChunk` 和 `confirmedChunks` 使用绑定参数；块记录及会话状态更新处于同一 SQLite 事务。独立测试覆盖重开查询、重复块拒绝及回滚。尚未接入 UploadStorage/网络业务，因此不是端到端续传。
- [x] M3查询协议起点：新增严格的 QUERY_SESSION 会话 ID 编解码及协议单测；服务端尚未处理 type 7，客户端尚未调用，不能视为查询功能可用。

M3事务基础验证（2026-10-31）：隔离目录 `/tmp/chunkharbor-m3-cpp` 重新构建后运行完整 CTest，**28/28通过**。首次测试使用早于创建时间的模拟时间戳，触发 `updated_at_ns >= created_at_ns` 约束而失败；改用当前纳秒时间后重跑通过。未执行服务端 kill -9/客户端重启续传验收，M3整体未完成。

M3第二子任务验证（2026-10-31）：虚拟机原工程有未提交改动，复制源码到 `/tmp/chunkharbor-m3-cpp` 隔离目录并放入本轮文件。虚拟机 SQLite 运行库已有，开发包缺失；用 `apt download libsqlite3-dev` 解压到 `/tmp/chunkharbor-sqlite-dev/extracted`，未修改系统安装。隔离目录执行 `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DSQLite3_INCLUDE_DIR=/tmp/chunkharbor-sqlite-dev/extracted/usr/include -DSQLite3_LIBRARY=/usr/lib/x86_64-linux-gnu/libsqlite3.so.0 -DOPENSSL_INCLUDE_DIR=/home/ning/ChunkHarbor/build/deps/openssl/usr/include -DOPENSSL_CRYPTO_LIBRARY=/usr/lib/x86_64-linux-gnu/libcrypto.so.3 -DCMAKE_CXX_FLAGS=-I/home/ning/ChunkHarbor/build/deps/openssl/usr/include/x86_64-linux-gnu`，随后 `cmake --build build -j 2` 与 `ctest --test-dir build --output-on-failure`，**28/28通过**。首次配置缺 OpenSSL 开发头，补现有私有缓存 include 后通过。未测试 Windows（该模块 Linux only）、ASan/Release、实际业务数据库事务及重启续传。

M3首个子任务验证（2026-10-31）：Windows `python tests/unit/schema_v1_test.py src/storage/schema_v1.sql` 通过；将这两个文件复制到虚拟机 `/tmp/chunkharbor-m3-schema-check/` 后执行同一 Python 测试通过。虚拟机正式目录执行 `cmake --build build -j 2` 与 `ctest --test-dir build --output-on-failure`，原有 M2 回归 26/26 通过；该目录未同步本轮 M3 文件，因此未运行新增 `schema_v1_test` 的 CMake 集成。未执行本轮完整重新配置/构建、真实 SQLite C++ 集成、重启续传或故障注入；这些不能计为通过。

- [x] M2：严格COMMIT_UPLOAD/COMMIT_RESULT及业务ERROR；真实CLI流式上传、同源FD身份检查、sent/confirmed区分；UploadHandler逐连接状态/请求关联、有界DiskExecutor业务投递及读取背压。
- [x] M2：UploadStorage实际块SHA-256与完整pwrite、fdatasync数据和同步追加回执后才ACK、整文件SHA-256、NOREPLACE发布及目录同步；同块/提交幂等、目标冲突与配额、断连保留。最低持久回执不等于SQLite/M3恢复。
- [x] M2：26项Ubuntu回归包含实际上传和恶意CLI响应、FIFO启动拒绝、容量1的粘包异步续作；独立审查发现问题后补红/绿测试并复核关闭。
- [x] M2可选：Qt6.8.3 MainWindow/QtTcpTransport连接状态原型，异步5秒握手、有界接收和错误状态；Windows实际构建/CTest 8/8通过，含7种Qt网络场景，截图已检查。没有GUI上传能力。
- [x] M2：新增Linux专用chunkharbor_server_core和TempFileStore，根FD固定、openat2受限解析、0600/CLOEXEC匿名O_TMPFILE、UniqueFd生命周期；路径/符号链接/目录替换竞态、故障注入和资源回归，见 `docs/TEMP_FILE_STORE.md`。M2实际缺口见 `docs/M2_REMAINING.md`。
- [x] M2：CHUNK_ACK五字段严格JSON、固定durable:true、ID/摘要hex互转、uint64 offset及长度/溢出检查、待确认DATA_CHUNK匹配；已接实际持久回执发送，见 `docs/CHUNK_ACK.md`。
- [x] M2：UPLOAD_PLAN三字段严格JSON、16字节ID与32lowerhex互转、块大小和uint64半开区间验证、512区间限制；不枚举块，安全计算文件块数并核对请求块大小。已接业务会话，见 `docs/UPLOAD_PLAN.md`。
- [x] M2：握手共享解析入口显式拒绝原始NUL，阻止合法JSON前缀隐藏尾随数据；HELLO/ACK/ERROR各类尾缀、前置/中间NUL、转义NUL、正常空白和4096字节边界单测，以及服务端/CLI集成回归。详见 `docs/HANDSHAKE.md`。
- [x] M2：BEGIN_UPLOAD严格五字段JSON；uint64文件大小/int64纳秒时间精度；UTF-8相对逻辑路径词法限制；块大小/小写摘要格式；拒绝重复/未知/缺失/错误类型/嵌套/超限/原始NUL尾缀。契约、设计取舍和命令见 `docs/BEGIN_UPLOAD.md`，不含文件或会话创建。
- [x] M2：DATA_CHUNK 60字节业务头显式大端编解码；完整长度、总payload上限、空数据、offset溢出检查；独立块对齐/整块/末块/文件范围检查。拥有解码数据；不引入OpenSSL/Qt/POSIX依赖。golden vector及FrameCodec组合测试，契约见 `docs/DATA_CHUNK.md`。
- [x] M2：独立可移植 chunkharbor_hash target，EVP 上下文 RAII、32字节摘要/64字符hex、内存块/增量/64KiB流式文件哈希；已知向量、二进制、读取边界和失败路径测试。约定与精确构建命令见 `docs/SHA256.md`。
- [x] M2：有界DiskExecutor、std::any结果与exception_ptr异常、独立eventfd完成通道、stop排空/析构取消及并发提交回归；约定见 `docs/DISK_EXECUTOR.md`。
- [x] M2首个子任务：严格HELLO/ACK/握手ERROR、版本/能力协商、客户端关联校验、逐连接握手状态、独立CLI hello与server --handshake模式。契约见 `docs/HANDSHAKE.md`。
- [x] M0：CMake、基础 RAII/日志、Linux server/CLI 入口。
- [x] 现有 Buffer：受限容量、整理/扩容、二进制流、5000次参考模型操作。
- [x] 单线程 LT EventLoop、EpollPoller、Channel，eventfd 跨线程唤醒；EINTR 重试、quit-before-run、有界 pending 队列（默认4096项）。
- [x] Channel weak owner 绑定和整个派发期间的强引用，回调移除后停止后续派发；生命周期回归测试。
- [x] TcpConnection：非阻塞读写、部分发送、EAGAIN/EINTR、按需 EPOLLOUT、有界输入/输出、读暂停恢复、半关闭排空。
- [x] TcpServer：loopback accept、连接上限、timerfd 空闲回收、资源不足暂停 accept、stop立即释放监听端口。
- [x] 纯 C++17 FrameCodec：32字节大端头、逐帧增量解析、长度/版本/类型/标志检查；Ubuntu与Windows相同golden vector。
- [x] 分离 protocol/client_core/linux_net targets，平台选项，Windows不编译Linux源码。
- [x] 可运行 Echo 服务端和非阻塞 Linux CLI；signalfd 受控停止。
- [x] 网络集成：100个同时连接、半包/粘包、1MiB帧、非法帧、EOF/RST、快速连断、慢读、FD恢复、空闲/数量上限、CLI超时。
- [x] 独立代码审查；修复停止后仍占端口的建议并自动化CLI沉默/截断响应超时场景。

## 本轮真实验证

M2完整上传本轮：Ubuntu正式目录Debug/ASan+UBSan/Release配置、构建与CTest均 **26/26通过**；Windows MinGW13.2纯协议 **7/7通过**、含hash **8/8通过**。精确使用/构建/测试命令见M2_USAGE.md，验收矩阵见M2_VALIDATION.md；Linux OpenSSL沿用已有私有开发头缓存。新增协议测试最初观察到的是缺header的编译失败，未冒充behavior-red；网络旧程序upload选项失败和应用暂停行为失败已实测。FIFO构造超时、容量1回调续作BUSY两个审查问题均先复现红测，再修复通过。receipt短写/EINTR/ENOSPC、数据及回执同步、发布后目录同步错误均有注入测试；不等同真实磁盘满/掉电。未运行TSan、真实断电/kill -9恢复、吞吐基准；SQLite/M3及完整GUI未实现。未提交/推送。以下为历史结果。

可选Qt原型：本机Qt6.8.3匹配MinGW Kit、Ninja、GUI=ON、server/CLI/hash=OFF，配置/构建/CTest **8/8通过**；真实截图无裁切。Qt独立只读审查未发现阻断问题。配置命令及启动SSH转发方法见M2_USAGE.md，未宣称GUI可上传或可分发打包。

Windows Qt→SSH本地转发→Ubuntu正式build服务端真实握手也通过：GUI --probe-port23587 经本地隧道至VM随机35087端口的--handshake服务，退出0；独占临时helper/服务进程经归属核对后已停止。此证据仅为连接握手，不是GUI文件上传。

TempFileStore前轮：Ubuntu正式目录Debug CMake配置/构建/CTest **20/20通过**；ASan/UBSan配置/构建/CTest **20/20通过**。Windows纯协议配置/构建/CTest **6/6通过**，未编译Linux组件；该轮未再运行Windows含hash配置。新增暂存单测先确认空实现失败，再接入实现通过。VM内核6.8，/tmp ext文件系统，支持openat2及O_TMPFILE；unsupported/EMFILE/EAGAIN/fchmod/fstat失败和EINTR通过链接包装注入覆盖。命令见TEMP_FILE_STORE.md。该轮未执行Release/TSan/GUI、真实bind mount/磁盘满、断电/恢复、发布或性能基准，未提交/推送。

本轮独立静态审查未发现阻断问题；根目录UID不匹配分支未做专门fstat注入，权限拒绝已测试。代码、测试及文档已同步正式虚拟机目录。

CHUNK_ACK前轮：新增单测先在空实现上canonical ACK断言失败，再接入实现。Ubuntu正式目录Debug CMake配置/构建/CTest **19/19通过**；ASan/UBSan配置/构建/CTest **19/19通过**。Windows MinGW13.2纯协议配置/构建/CTest **6/6通过**、含hash配置/构建/CTest **7/7通过**。精确命令见CHUNK_ACK.md（OpenSSL沿用既有配置）。该轮未运行Release/TSan/GUI、性能基准、GitHub Actions、落盘/断电/kill -9恢复测试；这些未实现，不能用该轮协议单测代替持久性证据。未提交/推送。

本轮独立审查未发现阻断问题，并用Windows既有构建执行ACK专项1/1通过；代码、测试与文档已同步正式虚拟机目录。

UPLOAD_PLAN前轮：Ubuntu正式目录Debug CMake配置/构建/CTest **18/18通过**；ASan/UBSan配置/构建/CTest **18/18通过**。命令见UPLOAD_PLAN.md，OpenSSL沿用已有缓存。Windows MinGW13.2纯协议配置/构建/CTest **5/5通过**，含hash配置/构建/CTest **6/6通过**。测试初稿因initializer_list混合const char*和string编译失败，修正后空实现按预期在golden断言失败，接入实现后通过；最终无测试失败。独立只读静态审查未发现阻断问题。该轮未运行Release/TSan/GUI、性能基准、GitHub Actions；未提交/推送。

握手NUL修复前轮：Windows先执行新增单测，确认3类消息共9个非法尾缀被接受；Ubuntu修复前 `ctest --test-dir build -R handshake --output-on-failure` **0/2通过**（单测及服务端集成均失败）。在共享parse入口增加显式NUL扫描后，Ubuntu Debug CMake配置/构建/CTest **17/17通过**，ASan/UBSan配置/构建/CTest **17/17通过**；Windows MinGW13.2纯协议 **4/4通过**、启用hash **5/5通过**。配置和全量命令沿用BEGIN_UPLOAD.md，专项命令为 `ctest --test-dir build -R handshake -V`。该轮未运行Release/TSan/GUI/性能基准/GitHub Actions；未提交/推送。

本轮独立只读审查通过，未发现新增阻断问题；代码、测试和文档已同步正式虚拟机目录。

BEGIN_UPLOAD前轮：Ubuntu正式目录Debug配置/构建/CTest **17/17通过**；ASan/UBSan配置/构建/CTest **17/17通过**。命令见BEGIN_UPLOAD.md，OpenSSL沿用前轮缓存；Windows MinGW13.2不启用hash配置 **4/4通过**，启用hash配置 **5/5通过**。先确认空实现的golden断言失败，再接入实现。独立审查发现原始NUL可绕过JSON库尾部检查，新增3类回归先复现失败，再在BEGIN_UPLOAD解析前拒绝原始NUL并重跑完整测试。该轮Release/TSan/GUI/性能基准、GitHub Actions未执行；未提交/推送。

前轮审查另用Windows临时探针确认原有 `decodeHello(encodeHello(Hello{}) + NUL + garbage)` 会接受；该历史问题现已通过本轮共享入口修复和单元/集成回归关闭。

DATA_CHUNK 前轮：Ubuntu正式目录 Debug CMake配置/构建及CTest **16/16通过**；ASan/UBSan配置/构建及CTest **16/16通过**。命令为 `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`、`cmake --build build -j 2`、`ctest --test-dir build --output-on-failure`；ASan同样操作build-asan并启用 `-DCHUNKHARBOR_SANITIZERS=ON`、设置检测环境变量。OpenSSL沿用前轮缓存的本地开发包配置，精确步骤见SHA256.md。Windows MinGW13.2：不启用OpenSSL的protocol配置构建/CTest **3/3通过**，启用hash配置构建/CTest **4/4通过**；命令见DATA_CHUNK.md/SHA256.md。新增测试先在空实现上golden vector断言失败，再接入实现通过。该轮Release/TSan/GUI/性能基准、GitHub Actions未执行；未提交/推送。

本轮 DATA_CHUNK 独立只读静态审查未发现阻断或重要问题；代码与文档已同步正式虚拟机目录。

SHA-256 前轮（2026-10-31）：Ubuntu 正式目录 CMake Debug 配置、构建及 CTest **15/15通过**；ASan/UBSan 配置、构建及 CTest **15/15通过**；Windows MinGW13.2 / OpenSSL1.1.1k Debug 构建及 CTest **3/3通过**。Linux 使用系统 OpenSSL3.0.2 运行库与无 root 解压的匹配开发包，未更改系统安装；配置路径及精确命令见 `docs/SHA256.md`。测试先以空实现执行并确认摘要断言失败，再接入 EVP 后通过。Windows 默认协议配置仍不要求 OpenSSL，显式 BUILD_HASH=ON 才构建哈希。该轮未运行 Release/TSan/GUI、EVP 提供者失败/OOM注入、性能基准；未接上传。

独立静态审查未发现阻断问题；补充读取有效前缀后失败、eofbit异常掩码和中文文件名测试。Linux CI 增加 libssl-dev 安装步骤，但本轮未推送，GitHub Actions 未执行。

DiskExecutor前轮：Ubuntu正式目录CMake配置、构建、CTest **14/14通过**；ASan/UBSan配置、构建、CTest **14/14通过**。新增disk_executor_test覆盖有界拒绝、多生产者、结果异常、线程归属、停止/取消、回调异常和普通loop队列满。Windows、Release、TSan该轮未执行（执行器为Linux组件）；下方握手/M1结果是此前历史。

本轮HELLO在Ubuntu执行CMake配置、构建、CTest：**13/13通过**；ASan/UBSan配置、构建及CTest：**13/13通过**；Windows MinGW13.2共享库构建及CTest：**2/2通过**。新增handshake_test、handshake_integration；原M1全部测试回归。精确构建命令沿用README，新增运行命令见HANDSHAKE.md。本轮Release/TSan/GUI未执行，以下11项结果为此前M1验收历史。

平台：Ubuntu 22.04，GCC 11.4，CMake 3.22.1，Python 3.10.12；正式目录 `/home/ning/ChunkHarbor`。

- Debug：`cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug`；`cmake --build build -j 2`；`ctest --test-dir build --output-on-failure`：**11/11通过**。
- ASan/UBSan：`cmake -S . -B build-asan -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCHUNKHARBOR_SANITIZERS=ON`；构建后设置 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1` 运行CTest：**11/11通过**。
- Release：`cmake -S . -B build-release -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release`，构建与CTest：**11/11通过**。
- Windows MinGW 13.2 / Ninja：禁用 server/CLI/GUI，构建 protocol/client_core 与协议测试：**1/1通过**。精确命令与验收对应见 `docs/M1_VALIDATION.md`。
- 原有 Buffer 的5项测试及之前事件循环的4项测试，是早期历史结果；当前以11项为准。

## 未验证与边界

- 用户人工“Windows CLI 中断→重启 Ubuntu 服务端→Windows CLI 继续上传”验收已通过。自动化另覆盖服务端 SIGKILL 后查询续传、Windows CLI 中断后状态文件续传与真实 SSH 隧道上传。
- 未做真实性能基准、TSan、硬件掉电和物理磁盘满测试；故障注入不等于真实硬件恢复。Windows GUI 上传、增量备份、下载恢复、TLS/认证未实现。
- 单线程 LT Reactor 保留，SQLite 事务只由有界磁盘工作池执行；启动核验在监听前完成。每连接资源上限及单文件64MiB/65536块限制沿用M2；服务端仅监听127.0.0.1。
- 数据库和文件系统不是一个原子事务；`COMMITTING` 启动核验后才协调状态。异常数据隔离在 `.quarantine`，需运营者检查；30天未更新的 CREATED/RECEIVING 会话启动时自动清理其暂存文件。无历史快照和正式文件总容量配额。
- 旧M2无数据库记录的未完成会话无法自动升级续传，其目录在M3首次启动时隔离而非删除。根目录须私有且受信任，不对抗恶意本地 root/同UID 进程。

## 下一步（仅一项）

- [ ] M4下一最小任务：先设计固定块 manifest 与增量计划的协议及可测边界，再接入无变化零 `DATA_CHUNK` 场景。
