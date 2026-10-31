# ChunkHarbor 开发进度

- 当前阶段：M1/M2已完成；文件上传闭环通过验收，另完成Windows Qt最小连接/握手原型。M3重启续传与完整GUI上传未实现。
- 最近更新：2026-10-31
- 本轮范围：按用户授权一次完成M2必需任务并补可选Qt连接原型，保留已有Reactor及前轮全部改动。

## 发布检查（2026-10-31）

- 用户授权提交并推送 M2；README 的 Ubuntu 示例改为通用仓库路径和独立演示目录。
- 本轮 SSH `miniob-ubuntu` 连接成功，主机名 `ning-virtual-machine`；执行 `cmake --build build -j 2` 及 `ctest --test-dir build --output-on-failure`，Ubuntu Debug 构建成功，26/26 通过。

## 已完成

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

- 未做性能基准、TSan、Windows GUI文件上传、TLS/认证、真实掉电恢复；不宣称吞吐量提升。
- 单loop是本轮完成的M1基线；多线程Reactor为后续扩展，不以100连接功能测试推断性能。
- 独立--upload模式已做文件写入和最小持久回执；SQLite、重启查询/续传、固定块增量和下载恢复仍未实现。Echo保留不透明字节语义。
- 文件64MiB/65536块、32个保留会话/512MiB预留payload、32默认/64最大连接、2worker/64outstanding。正式文件总容量、内核缓存和元数据开销不计入上述payload限额。无分页或自动清理未完成会话。
- 根目录须私有且受信任，openat2/O_TMPFILE和/proc必需；不会弱化fallback，不对抗恶意本地root/同UID进程。协议路径检查与实际目录FD约束配合，Windows恢复路径兼容留后续。
- ACK在数据和回执同步成功后发送，但不表示整文件已发布；COMMIT_RESULT才是客户端最终成功。M2回执非SQLite事务。发布/清理中断可能留下需要人工检查/M3协调的状态，启动保守拒绝，不能称为崩溃恢复。
- CLI同FD身份检查不等于文件系统快照；Windows OpenSSL旧SDK仅兼容性验证，不作为部署版本建议。Qt原型只做连接握手，不上传。
- 服务端只监听127.0.0.1。每连接输入上限1MiB+32，输出4MiB；这些限制不含内核缓冲、短暂副本和allocator开销。
- 所有连接注册/操作/最终销毁归属loop线程；跨线程投递调用方须保证EventLoop仍存活，队列满抛length_error。
- 旧验证目录 `/home/ning/ChunkHarbor-verify-20261030` 不作正式开发目录；原目录备份仍保留。

## 下一步（仅一项）

- [ ] M3：先确定SQLite持久会话schema及发布窗口协调设计，再实现查询/重启续传；本轮不自动推进。
