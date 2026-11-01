# M4 固定块增量备份开发计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task; use superpowers:subagent-driven-development only if the user chooses delegation. Steps use checkbox (`- [x]`) syntax for tracking.

**Status:** 2026-11-01任务1–13功能实现及自动验收完成；最终命令、审阅修复及过程偏差见 `docs/M4_VALIDATION.md` 和 `docs/PROGRESS.md`。下方勾选表示最终交付，部分初版未按严格先红后绿顺序执行，不把此类过程补写为已发生。

**Goal:** 已成功备份的文件再次备份时只传变化块，并提供安全的版本发布、目录批任务和只读差异预览。

**Architecture:** 保留自主Linux Reactor及有界磁盘工作池；protocol负责确定性消息，client_core负责可移植扫描/计划状态，平台CLI负责传输和本地状态适配。服务器固定不可变基线，分页接收摘要，复制相同块并接收变化块，在整文件校验和版本前置条件检查后发布；预览与执行共用差异算法。

**Tech Stack:** C++17、CMake、SQLite WAL/FULL、OpenSSL EVP（服务端）、Qt 6 Network（Windows CLI）、现有epoll/EventLoop/DiskExecutor、Python集成测试。

**Spec:** `CODEX_DEVELOPMENT_GUIDE.md` 第5.5、6、7.1节及M4条目；首个模块细节见 `docs/superpowers/specs/2026-11-01-m4-manifest-page-design.md`。本计划包含后续模块的建议设计；尚未实施，不能作为功能完成证据。

## Global Constraints

- 按用户确认进入M4，不重做M0–M3；用户最新明确授权本轮完成M4全部剩余任务；不跨入M5–M7。
- C++17；protocol/client_core没有Qt、Linux系统调用依赖；服务端不依赖Qt。
- 保留32字节大端帧头、64KiB控制payload、DATA_CHUNK总payload上限1MiB及现有golden vectors；真实块上限使用`MaxChunkDataSize`。
- 本阶段继续单文件64MiB、65536块、32个保留上传会话及512MiB上传预留数据上限；不先扩大容量或加入性能优化。
- MANIFEST_PAGE每页1..512个摘要，最多128页；分页计划每页最多512个区间，最多64页（65536块最多32768个非相邻缺失区间）。
- 不变文件0 DATA_CHUNK；“0数据块”仍有扫描、哈希、清单和控制通信。固定块比较不称为rsync。
- 文件系统与SQLite不是原子事务；任何成功确认都必须有对应持久化边界和可重放恢复规则。
- 服务端仅loopback/SSH；受限目录FD和安全openat2解析，不用字符串前缀、符号链接或硬链接绕过隔离。
- 不自动提交/推送；不覆盖用户备份、不广泛删除。所有迁移、崩溃和破坏性测试只针对新建隔离测试根目录。
- 不纳入M5完整GUI、M6下载、M7 TLS/认证，以及历史版本浏览、定时备份、rolling checksum、独立空目录保存或删除传播。

## Review Focus

1. 相同内容发生A→B→A变化也不能绕过版本条件：使用随机version ID，不以整文件SHA作为版本ID；测试归任务4/8。
2. 旧M3已提交会话不能在更新后错误地校验当前文件：导入不可变版本并保留会话到版本映射；测试归任务5。
3. 发布或清理任一步中断后仍可启动，旧版或新版必须完整可辨：故障注入及进程SIGKILL分别验证；测试归任务5/8。
4. 预览不得占用上传排他锁或污染确认块，执行时基线/源文件变化必须重算或拒绝；测试归任务6/12。
5. 目录扫描遇到链接、路径编码、权限失败、取消与客户端重启：不能逃逸源根，也不能把部分失败报告为全成功；测试归任务10/13。

## 实施顺序与设计决定

依赖主线：1 → 2 → 3 → 4 → 5 → 6 → 7 → 8 → 9 → 10 → 11 → 12 → 13。这里的数字是子任务，不是要求一轮全部完成。若某任务需要拆分，以其下独立验收边界为准。

### A. 版本与兼容

以下为本计划建议，先在任务2固定契约，再接业务：

- 增量能力名`fixed_block_delta_v1`，保留已有`upload`流程和严格五字段BEGIN_UPLOAD；未协商能力的连接禁止新消息。
- 预留18–21承载manifest及plan；新增24–31，22/23继续留给AUTH。扩展既有wire v1的消息集合，不改变已有帧/消息；旧端不协商能力则不发送扩展消息。修改FrameCodec识别范围时保留未知编号拒绝测试。
- 不通过放宽旧BEGIN_UPLOAD字段来开启覆盖。新增增量入口，并把期望基线绑定到会话；没有当前版本时只能声明“新建”。
- version ID为随机128位小写hex，与session ID类型别名分开；内容相同的两次修改仍有独立身份。不变文件可直接复用当前version ID，不制造重复内容版本。
- chunk size不匹配时退化为全量差异计划；首版不为旧内容做任意重新分块匹配。

### B. 拟定消息流程

任务2应将下表转为严格字段文档和编解码测试，完成后冻结；已通过明确Ruling追加change_kind。

| type | 消息 | 核心字段/行为 |
| --- | --- | --- |
| 24 | BEGIN_DELTA | metadata为现有五字段上传元数据；mode为upload/preview；base_version为`auto`、null（断言目标不存在）或明确version ID |
| 25 | DELTA_READY | session_id、确定的base_version（null或ID）、block_count；服务端选择auto后必须固定，不能比较中途换基线 |
| 18 | MANIFEST_PAGE | 沿用首个模块草案的四字段 |
| 19 | MANIFEST_DONE | session_id、page_count、block_count；必须与连续完整清单一致 |
| 26 | MANIFEST_ACK | session_id、next_page_index、phase（COLLECTING/PLAN_READY）；每页及DONE各一应答，页确认只代表元数据持久，不代表数据块已确认 |
| 27 | GET_PLAN_PAGE | session_id、page_index；一问一答拉取计划，避免大批响应堵塞输出缓冲 |
| 20 | UPLOAD_PLAN_PAGE | session_id、base_version、page_index、missing_ranges；半开区间、升序、不重叠且非相邻（跨页也合并） |
| 21 | UPLOAD_PLAN_DONE | session_id、base_version、page_count、block_count、missing_blocks、missing_bytes、reused_blocks、reused_bytes；客户端取完最后页后再请求page_count获得DONE |
| 28 | QUERY_DELTA | session_id；恢复或查询phase/进度 |
| 29 | DELTA_STATUS | session_id、mode、phase、base_version、next_manifest_page、missing_bitmap、result；按phase固定字段，result仅COMMITTED时为完整提交结果，其余null |
| 30 | COMMIT_DELTA | session_id；只允许upload模式，重复提交返回同一结果 |
| 31 | DELTA_RESULT | session_id、version_id、path、size、sha256、committed:true |

`DATA_CHUNK/CHUNK_ACK`复用type5/6，但仅在新会话的TRANSFERRING阶段接受；不能把旧会话与增量会话混用。ERROR沿用type15和已有稳定码（MALFORMED_MESSAGE、CONFLICT、BUSY等），无需为了阶段不同新增宽泛错误类别。

phase固定为COLLECTING、PLAN_READY、TRANSFERRING、VERIFYING、COMMITTING、COMMITTED、FAILED、EXPIRED。DELTA_STATUS在非TRANSFERRING/VERIFYING阶段missing_bitmap固定为空；在这两个阶段返回M3相同编码的缺失位图。COMMITTING恢复完成前返回BUSY，不能让客户端据此再次上传。计划在完成前重连仍可重新拉取；TRANSFERRING阶段进度以查询位图为准。

MANIFEST_DONE成功前须完成持久差异计划并进入PLAN_READY。upload模式最后一次GET_PLAN_PAGE（page_index等于page_count）先调用prepareUpload，持久化复用块并进入TRANSFERRING，再发送UPLOAD_PLAN_DONE；preview模式只返回计划统计并保持PLAN_READY。新上传必须收到完整DONE；断线恢复到TRANSFERRING时，已核对源身份的DELTA_STATUS位图作为完整恢复依据，不要求重走最初计划。PLAN_READY阶段没有传输进度位图，重连应继续拉取计划。

页请求使用连接内非零递增request_id，应答逐一匹配。相同页内容在新request_id下重试可幂等成功；相同页号不同摘要报CONFLICT；跳页报MALFORMED_MESSAGE。结构validator仍校验“下一新页”，重复页由持久会话层先判定，不能跳过范围校验。断连不会视作MANIFEST_DONE。

preview会话不创建上传data、不复制块、不插入transfer_chunks、不锁定目标写入；允许持久化短期清单。最多8个preview会话、24小时后过期，其清单/数据库开销计入独立配额。upload保留30天期限；取消先停止调度并保存可恢复会话，显式销毁会话不是首版要求。

### C. 文件与数据库建议

- 新增`versions`、`version_chunks`、`current_files`、`delta_sessions`、`manifest_pages`、`manifest_chunks`、`delta_plan_ranges`；使用schema v2事务迁移，保留v1历史表。
- `.versions/<version_id>/data`为不可变内部对象，`files/<logical_path>`保留为最新可见完整副本。两者为独立文件，不用硬链接破坏现有st_nlink检查；代价是额外磁盘空间，M4不宣称物理块去重。
- v1已提交文件按真实内容校验并导入内部版本后，给旧session关联version ID；旧会话查询返回旧结果，旧会话启动核验读取旧版本对象。旧版客户端新建路径也接入同一版本登记，不能形成两套“当前版本”。
- 发布顺序：完整暂存校验 → 持久化不可变版本对象及目录 → SQLite记录COMMITTING意图（基线ID、新ID、路径） → 从新对象构造受限发布临时文件并fsync → 检查基线及目标身份 → 原子rename替换最新副本并同步目录 → SQLite事务CAS更新current_files及COMMITTED → 应答。
- 新建仍用NOREPLACE；更新只在根排他锁/目标锁持有且版本前置条件匹配时替换。正式文件本地外部修改也须在发布前通过身份/摘要核验拒绝，不能默默覆盖。
- 恢复遇到COMMITTING：最新副本为新摘要则补数据库；为旧摘要则从持久新对象重新发布；两者都不是则隔离/拒绝该目标，不能猜。检查新/旧内容相同的情形时以持久意图和版本ID判定，不能只靠摘要选择版本。
- 新对象写入失败无成功回执；未被记录引用的对象只能作为孤立对象隔离。历史内部版本本阶段不自动GC、不提供浏览；测试输出占用，明确空间随版本增长，不默认启用真实目录上的自动迁移。

## 任务1：MANIFEST_PAGE纯协议模块

**Files:** 新增`include/chunkharbor/protocol/ManifestPage.h`、`src/protocol/ManifestPage.cpp`、`tests/unit/manifest_page_test.cpp`；修改`CMakeLists.txt`、`docs/PROTOCOL.md`。

**Interfaces:** 采用首个草案的`ManifestPage`、`ManifestPageError`、`encodeManifestPage`、`decodeManifestPage`和`validateManifestPageForFile`，签名保持一致。

- [x] 写测试：1/512项往返、精确JSON及type18帧golden；0/513项、页128、错位起始、错误hex拒绝；0/1/511/512/513/65536块上下文、末块、uint64溢出、NUL/重复键/深层JSON/65536字节边界。
- [x] 注册测试，先以可编译空实现运行`ctest --test-dir <build> -R '^manifest_page_test$' --output-on-failure`，确认摘要/字段断言失败，区分编译失败与行为失败。
- [x] 实现拥有数据的单页编解码与上下文校验；无磁盘、Qt或OpenSSL依赖，不累计所有页。
- [x] Windows纯协议及Linux完整CTest通过后记录实际结果和可审阅差异。此时仅完成编解码，不广告增量能力。

## 任务2：剩余分页与增量协商协议

**Files:** 新增`docs/M4_PROTOCOL.md`、`include/chunkharbor/protocol/DeltaControl.h`、`src/protocol/DeltaControl.cpp`、`include/chunkharbor/protocol/PlanPage.h`、`src/protocol/PlanPage.cpp`；测试`delta_control_test.cpp`、`plan_page_test.cpp`；修改FrameCodec、CMakeLists。

**Interfaces:** 公共别名`SessionId/VersionId = std::array<uint8_t,16>`（不同字段不得混用）；`BeginDelta{BeginUpload metadata; DeltaMode mode; BaseSelector base;}`、`DeltaReady`、`ManifestDone`、`ManifestAck`、`GetPlanPage`、`PlanPage`、`PlanDone`、`DeltaStatus`、`DeltaResult`分别一对encode/decode；`BaseSelector`用enum Auto/Absent/Exact和Exact对应ID，不以空字符串混淆null。

- [x] 编写并审阅M4_PROTOCOL的每类严格字段/枚举、阶段矩阵、错误、字节上限；同步本计划B表，解决字段冲突后再实现编解码。
- [x] 写测试验证页关联、缺页/重复/乱序策略、空计划DONE、跨页区间合并、512/513区间、32768交错差异、多phase STATUS、64位整数精度和旧消息golden不变。
- [x] 实现各类编解码和完整计划校验器，编码不能构造无界向量。初版未执行空实现红测；追加change_kind契约确有先失败后通过记录。
- [x] Windows/Linux同一golden通过；业务处理器继续拒绝未启用扩展，不提前宣告支持。

## 任务3：可移植固定块差异计算

**Files:** 新增`include/chunkharbor/client_core/DeltaPlanner.h`、`src/client_core/DeltaPlanner.cpp`、`tests/unit/delta_planner_test.cpp`；注册client_core测试。

**Interfaces:** `BlockDescriptor{uint32_t index,length; array<uint8_t,32> sha256;}`；`enum class BlockAction{Reuse,Upload}; BlockAction compareBlock(const BlockDescriptor& source,const std::optional<BlockDescriptor>& base);`。`PlanTotals`包含uint64缺失/复用块数及字节数；区间由`RangePageBuilder::push(uint32_t index)`流式合并，`takePage()`每次至多512区间，`finish()`封闭最后页。

- [x] 写测试：无基线全传、全相同零缺失、单块变化、末块长度不同、增长/缩短、不同块大小全传、头部插入后结果正确；相同摘要不同长度不能Reuse。
- [x] 写65536交错块输入，断言32768区间、64页、无单页超512；大小统计按末块真实长度。
- [x] 实现不依赖I/O的比较与流式分页；同一函数供服务端和预览使用，避免两套算法。
- [x] 双平台单测通过；不从此推断网络/存储节省量。

## 任务4：schema v2、版本身份与当前指针

**Files:** 新增`src/storage/schema_v2.sql`、`tests/unit/schema_v2_test.py`；修改`SqliteStore.h/.cpp`、`sqlite_store_test.cpp`；新增`docs/M4_STORAGE.md`。

**Interfaces:** `VersionRecord{id,path,size,chunkSize,sha256}`；`SqliteStore::currentVersion(string_view path)->optional<VersionRecord>`、`registerVersion(const VersionRecord&)`、`compareAndSetCurrent(string_view path,const optional<string>& expected,string_view replacement)`；CAS必须位于调用方提交事务内，失败不能部分更新。

- [x] 写空库→v2、v1→v2、重开、未知版本拒绝、迁移故障回滚、外键/活跃目标唯一约束测试。修改v1的CHECK(version=1)表必须在迁移事务内重建，不能只更新其值。
- [x] 写A→B→A摘要相同但ID不同的CAS测试；旧ID不得更新当前路径。
- [x] 实现绑定SQL和事务；一并定义会话到版本映射及所有表的状态/配额约束。
- [x] SQLite单测通过；此任务不移动真实文件、不接受增量网络请求。

## 任务5：不可变版本存储及旧数据导入

**Files:** 新增`include/chunkharbor/storage/VersionStore.h`、`src/storage/VersionStore.cpp`、`tests/unit/version_store_test.cpp`；修改UploadStorage启动核验及M3兼容回归。

**Interfaces:** `VersionStore::importLegacy(const VersionRecord&,int sourceFd)`、`openVerified(string_view versionId)->UniqueFd`、`publishObject(string_view versionId,int verifiedTempFd)`；全部由工作线程/监听前启动阶段调用，FD返回值拥有资源。

- [x] 测试旧COMMITTED导入、导入中断重试、目标更新后旧session仍返回旧结果、内容损坏拒绝；根目录/版本目录符号链接与特殊文件拒绝。
- [x] 同步补上已记录的M3清理缺口回归：COMMITTED/EXPIRED清理到一半缺meta仍能依据可信数据库安全收尾；不能因此放松活跃会话元数据核验。
- [x] 实现受限复制/fsync/目录同步与可重试导入，更新旧会话核验路径；不在EventLoop执行哈希或复制。
- [x] 通过隔离迁移与M3上传/续传全回归；新v2库不能再交给旧二进制使用，记录升级边界。真实用户根迁移须另行授权并先备份。

## 任务6：持久manifest、阶段和只读会话

**Files:** 新增`include/chunkharbor/storage/DeltaSessionStore.h`、`src/storage/DeltaSessionStore.cpp`、`tests/unit/delta_session_store_test.cpp`；扩展SqliteStore仅添加任务4已定义的表接口。

**Interfaces:** `begin(const BeginDelta&)->DeltaReady`、`appendManifest(const ManifestPage&)->ManifestAck`、`finishManifest(const ManifestDone&)->ManifestAck`、`status(const SessionId&)->DeltaStatus`。会话绑定源metadata/base/version/mode，phase和next_page持久化。

- [x] 测试页事务提交重开、完全相同页幂等、不同内容CONFLICT、跳页拒绝、DONE页数/块数不符、空文件0页、断连不完成清单。
- [x] 测试preview不创建上传data、不产生确认块、不锁目标；8个preview及第9个拒绝，24小时到期；upload沿用32/512MiB限制。
- [x] 实现每页持久化、比较前置条件、事务级配额；不在内存持有65536条DOM或一连接全部请求。
- [x] 专项重启测试通过；只有页元数据可确认，此时没有可用的增量上传。

## 任务7：持久差异计划与安全复用块

**Files:** 新增`include/chunkharbor/storage/DeltaStorage.h`、`src/storage/DeltaStorage.cpp`、`tests/unit/delta_storage_test.cpp`。

**Interfaces:** `buildPlan(const SessionId&)`、`planPage(const SessionId&,uint32_t)->PlanPageOrDone`、`prepareUpload(const SessionId&)`、`write(const SessionId&,const DataChunk&)->ChunkAck`；`PlanPageOrDone=variant<PlanPage,PlanDone>`，计划由任务3算法生成并分页落库。

- [x] 写无变化/单块/增长缩短/不同块大小计划测试及preview与upload同输入同计划测试；最多64页且按需读取。
- [x] 写复用块被篡改、短读、pwrite短写/EINTR、fdatasync失败、SQLite失败不确认测试；preview禁止prepare/write。
- [x] 失败后从不可变基线按索引复制并重算摘要，持久化后才能标记复用块；传入块同时匹配manifest摘要，不能只检查客户端自带DATA_CHUNK摘要。
- [x] 崩溃后按块重验，未确认复制块仍缺失；计划构建工作完成前不进入PLAN_READY；upload模式准备持久复用块后才发送完整DONE，发送DONE不代表变化块已上传。

## 任务8：条件发布与崩溃恢复

**Files:** 新增`src/storage/DeltaRecovery.cpp`、`tests/integration/delta_recovery_test.py`；实现任务C发布流程，修改UploadStorage兼容适配及版本恢复文档。

**Interfaces:** `DeltaStorage::commit(const SessionId&)->DeltaResult`、`recoverPendingPublishes()`（监听前调用）；所有路径由同一根锁及目标锁保护，COMMITTED结果持久保存以便重复查询。

- [x] 写CAS冲突、A→B→A版本、未完成块、整文件摘要错、外部目标改变、重复commit、COMMIT_RESULT丢失后查询测试。
- [x] 覆盖四个精确发布SIGKILL屏障以及数据写入/同步、对象同步、发布目录同步、SQLite确认错误；旧版/新版都不能成为半文件。未穷举全部失败排列，不声称物理掉电验证。
- [x] 实现发布/启动协调；同内容不变任务校验基线后复用既有version并返回成功，无需重写最新文件。
- [x] 另用测试helper的明确进度屏障触发SIGKILL并重启，避免仅靠sleep碰窗口；校验旧历史和最新文件、数据库指针及重复操作。故障注入不能写成硬件掉电实测。

## 任务9：服务端增量协议状态机

**Files:** 新增`apps/server/DeltaHandler.h/.cpp`、`tests/unit/delta_handler_test.cpp`、`tests/integration/delta_protocol_test.py`；修改UploadHandler及server/main的构造接线，复用既有Reactor。

**Interfaces:** `makeDeltaHandler(DeltaStorage&,DiskExecutor&,EventLoop&)`返回既有MessageCallback；HELLO协商扩展，按任务2phase表派发。旧upload入口保持独立且不会接管增量session。

- [x] 测试未协商拒绝、旧客户端回归、所有phase非法请求、request_id错配、半包粘包、容量1队列、断连中回调、慢读拉取计划。
- [x] 失败后逐请求投递工作池并暂停连接读取，完成后安全回loop；回调用weak_ptr，连接销毁不取消已经持久提交的结果。
- [x] 接入任务6–8；暂存复制/哈希/SQLite不能进入loop，BUSY不伪装成功。
- [x] Python帧客户端完成“新建→不变→单块变化→服务端重启续传”，旧M1–M3全回归通过，才协商fixed_block_delta_v1。

## 任务10：客户端源快照与流式manifest

**Files:** 新增`include/chunkharbor/client_core/SourceSnapshot.h`、`src/client_core/SourceSnapshot.cpp`、`include/chunkharbor/client_core/DeltaExchange.h`、`src/client_core/DeltaExchange.cpp`、对应单测。

**Interfaces:** `SnapshotIdentity{uint64_t size;int64_t mtimeNs;array<uint8_t,32> sha256;}`；`ManifestReader::next()->optional<ManifestPage>`至多一页；`DeltaExchange`消费已验证Frame并产生下一请求，不操作socket。源读取/摘要通过平台适配接口提供，核心不引入Qt/POSIX。

- [x] 测试空文件、512/513/65536块、一页内存上界、读取中修改/截断、同大小修改、拒绝不完整计划及错误基线ID。
- [x] 源文件使用同一打开句柄建立快照，上传前/提交前复核整文件摘要和必要身份；不能只依赖mtime或路径；文件读取异常不能当EOF。
- [x] 实现串行请求状态机、流式页生成和取消检查；新增接口不直接绑定现有Linux专用UploadClient。
- [x] 双平台核心单测通过；Qt后续适配只能在socket所属线程访问socket，GUI主线程不做扫描/哈希。

## 任务11：Linux/Windows CLI单文件增量与恢复

**Files:** 新增`apps/cli/DeltaClient.h/.cpp`、`apps/cli/QtDeltaClient.h/.cpp`、`tests/integration/delta_cli_peer_test.py`、`tests/integration/windows_delta_ssh_test.py`；修改两个CLI入口和帮助。

**Interfaces:** 新命令`backup --file SOURCE --path LOGICAL --state STATE [--chunk-size N] [--port N]`；保留旧upload。状态包含端点、session/source metadata、mode、base_version、phase，以平台原子写入适配保存，不把uint64/int64经double序列化。

- [x] 先用恶意peer验证服务端错页/漏DONE/错ID/虚假结果必须拒绝，状态文件写失败不得开始数据传输。
- [x] 实现任务10状态机的Linux/Qt传输适配；恢复前核对源快照及端点，再按phase重传页或查询缺失块，不凭本地ACK数猜测。
- [x] 真实集成：首次全量、第二次0块、改1块只传1块；Windows中断、Ubuntu SIGKILL重启、Windows续传，最终两端SHA一致且发送块数精确。
- [x] 状态写入后断电持久性与普通进程重启区分记录；不同源/端点必须拒绝误用，成功移除状态，失败保留诊断。

## 任务12：可复用只读差异预览

**Files:** 新增`include/chunkharbor/client_core/BackupPreview.h`、`src/client_core/BackupPreview.cpp`、对应单测及`tests/integration/preview_test.py`；两个CLI添加`preview --file`入口。

**Interfaces:** `PreviewEntry{logicalPath,sourceIdentity,baseVersion,changeKind,missingBlocks,missingBytes,reusedBytes}`；changeKind=Added/Modified/Unchanged/Excluded/Error。`preview`使用任务2的mode=preview，`backup`接受预览生成的明确基线及源身份再执行，不信任缓存估计。

- [x] 测试预览前后正式文件、current_files和transfer_chunks不变；允许短期preview元数据变化，但不能冒充“整个数据库零写入”。
- [x] 测试相同快照/基线预览和upload计划完全一致；预览后源变化拒绝或重扫，目标基线变化返回CONFLICT。
- [x] 失败后复用任务3/6/7算法，只返回真实统计；hash不变但mtime变化应归Unchanged（mtime不是差异数据依据）。
- [x] CLI显示新增/修改/未变及预计传输字节；不把预览估计写为已节省流量，不实现GUI页面。

## 任务13：目录批任务、取消重试及M4总验收

本任务分三轮执行：13a扫描与队列；13b CLI接入与取消重试；13c全阶段验收，每轮独立可测。

**Files:** 新增`include/chunkharbor/client_core/DirectoryScanner.h`、`src/client_core/DirectoryScanner.cpp`、`include/chunkharbor/client_core/BackupBatch.h`、`src/client_core/BackupBatch.cpp`、单测及`tests/integration/directory_backup_test.py`、`docs/M4_VALIDATION.md`；修改CLI和README。

**Interfaces:** `DirectoryScanner::next()->optional<ScanEntry>`流式输出；`BackupBatch::step()`串行推进一个文件/状态，`cancel()`停止调度并保留可恢复记录；`BatchSummary`提供成功/失败/跳过/取消数量、计划/已确认字节。CLI增加`backup --directory ROOT --prefix LOGICAL --state-dir DIR`及`preview --directory`。

- [x] 13a：写扫描测试，最多10000文件、深度64、单文件64MiB，待处理内存队列最多128项；任务状态落盘，上述限额超出即报错，禁止静默截断目录。只纳入普通文件，不跟随符号链接/Windows reparse point；空目录不保留，Unicode转相对UTF-8，合法名转换失败则明确列Error。
- [x] 13a：定义最小排除规则为精确目录组件名列表（如`.git`、`build`），由用户显式指定，不实现完整gitignore；预览列Excluded，源根内状态目录须自动排除。写编码碰撞、文件扫描后替换为链接、权限拒绝的测试。
- [x] 13b：实现串行文件队列（M4不追求并发），每文件独立会话；取消停止新调度，文件错误列入汇总并允许明确重试。SIGINT测试中断后重新运行只恢复未完成文件；部分失败返回非零，不影响已提交文件。
- [x] 13b：目录预览汇总任务12结果；源文件删除仅列扫描/执行错误，不传播删除到服务端。首版不提供“整目录原子快照”，明确文件逐个提交。
- [x] 13c：执行下方完整验收矩阵，记录真实命令与输出；更新M4_VALIDATION、README和PROGRESS，未验证平台/失败项不勾完成。内部版本GC/总容量管理作为限制公开说明。

## 验证命令与最终验收

以下均为未来执行模板，**本计划编写时未执行构建或产品测试**。不要把测试个数固定为M3的30/9；新增后以CTest实际结果为准。

Linux独立构建目录（依赖按README实际安装/配置，不复制他机缓存路径）：

```bash
cmake -S . -B build/m4-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCHUNKHARBOR_BUILD_SERVER=ON -DCHUNKHARBOR_BUILD_CLI=ON -DCHUNKHARBOR_BUILD_GUI=OFF -DCHUNKHARBOR_BUILD_TESTS=ON
cmake --build build/m4-debug -j 2
ctest --test-dir build/m4-debug --output-on-failure
```

Windows（当前匹配Kit，执行前核对路径）：

```powershell
$env:PATH = 'D:\QT\Tools\mingw1310_64\bin;D:\QT\6.8.3\mingw_64\bin;' + $env:PATH
cmake -S . -B build-windows-m4 -G Ninja -DCMAKE_MAKE_PROGRAM='D:/CLion 2023.3.6/bin/ninja/win/x64/ninja.exe' -DCMAKE_CXX_COMPILER='D:/QT/Tools/mingw1310_64/bin/g++.exe' -DCMAKE_PREFIX_PATH='D:/QT/6.8.3/mingw_64' -DCHUNKHARBOR_BUILD_SERVER=OFF -DCHUNKHARBOR_BUILD_CLI=ON -DCHUNKHARBOR_BUILD_GUI=OFF -DCHUNKHARBOR_BUILD_HASH=OFF -DCHUNKHARBOR_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-windows-m4 -j 2
ctest --test-dir build-windows-m4 --output-on-failure
```

每项先专项红/绿，受影响完整回归通过后不无理由反复重跑；13c统一执行Linux Debug/ASan+UBSan/Release和Windows CLI、真实SSH端到端。ASan使用独立目录及`CHUNKHARBOR_SANITIZERS=ON`；TSan、物理掉电、磁盘满和性能基准未实际执行就明确列为未验证。

| 场景 | M4完成条件 |
| --- | --- |
| 初次备份/不变/改1块 | 全传/0 DATA_CHUNK/只传1个对应变化块；SHA一致 |
| 空文件/增长/缩短/头部插入 | 计划和真实内容正确，无旧尾部残留；不夸大插入优化 |
| 超过一页/交错变化 | manifest与计划可分页，DONE完整，内存/队列有界 |
| 服务端/客户端重启 | 清单/复制/传输/提交阶段均可明确恢复；无虚假ACK |
| 并发同路径/过期基线/外部修改 | 冲突拒绝，旧版本保留可校验 |
| 预览→执行 | 同基线同源时计划一致，变化则重查或拒绝 |
| 目录/取消/权限/编码 | 汇总真实，失败退出非零，可恢复，不逃逸源根 |
| M1–M3兼容及迁移 | 旧消息不变；旧会话历史可查；新库旧二进制拒绝 |

## 本轮记录与下一步

用户随后授权直接完成全部剩余M4。现已执行实现及验收，未提交/推送；未关闭旧冲突检查来打通增量，而是新增能力、基线和版本发布路径。实际文件组织合并、预览change_kind和配额决定见PROGRESS的Ruling。

协议/存储/客户端按独立文件域并行实施，由主代理统一CMake/网络接线和全平台验证，关键问题独立审阅并修复。最终Debug44/44、Release44/44、ASan42/42、Windows17/17及Windows SSH真实验收通过。后续为M5，另行按阶段推进。
