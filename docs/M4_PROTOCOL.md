# M4 协议契约

本文件冻结 fixed_block_delta_v1 的正文格式；可用性以服务端接线及集成验收为准。旧消息正文、32字节帧头及协议版本不变。18–21、24–31仅在HELLO协商能力后使用；22/23保留AUTH。每次请求使用连接内非零递增request_id，响应逐一匹配。

## 通用表示与严格校验

所有正文是UTF-8 JSON对象，最多65536字节。字段必须恰好符合下表，拒绝未知/重复键、原始及转义NUL、BOM、错误嵌套和非整数数值。对象最大深度4；数组受正文及512项计划/摘要上限限制。ID使用32位小写hex，SHA-256使用64位小写hex；ID模板不依赖哈希库。JSON整数直接使用int64/uint64，不经过double。编码字段采用JSON库字典序，测试固定golden。

session_id与version_id均为16字节值，各字段语义独立；版本ID由存储层随机生成，不取内容摘要。base_version在BEGIN_DELTA允许字符串auto、null或版本ID；其他消息只允许null或版本ID。null表示不存在基线。

## 严格字段

|编号|消息|全部字段|
|---|---|---|
|24|BEGIN_DELTA|metadata、mode、base_version|
|25|DELTA_READY|session_id、base_version、block_count|
|18|MANIFEST_PAGE|session_id、page_index、first_block_index、hashes（既有模块）|
|19|MANIFEST_DONE|session_id、page_count、block_count|
|26|MANIFEST_ACK|session_id、next_page_index、phase|
|27|GET_PLAN_PAGE|session_id、page_index|
|20|UPLOAD_PLAN_PAGE|session_id、base_version、page_index、missing_ranges|
|21|UPLOAD_PLAN_DONE|session_id、base_version、page_count、block_count、missing_blocks、missing_bytes、reused_blocks、reused_bytes、change_kind|
|28|QUERY_DELTA|session_id|
|29|DELTA_STATUS|session_id、mode、phase、base_version、next_manifest_page、missing_bitmap、result|
|30|COMMIT_DELTA|session_id|
|31|DELTA_RESULT|session_id、version_id、path、size、sha256、committed|

metadata严格复用BEGIN_UPLOAD的path/size/mtime_ns/chunk_size/sha256五字段。mode只能upload或preview。committed只能布尔true。size、块字节统计限制于64MiB；block_count最多65536。MANIFEST_DONE page_count必须为ceil(block_count/512)，空文件为0页。MANIFEST_ACK phase只能COLLECTING或PLAN_READY，next_page_index最多128。

missing_ranges为[[begin,end],...]半开块索引区间；单页1..512项，页号0..63，范围0..65536，必须升序、不重叠且不相邻。跨页同样不能相邻。GET_PLAN_PAGE允许page_index=64来请求64页后的DONE。零缺失对应0页，直接请求第0页取得DONE。DONE的缺失及复用块总和为block_count；字节总和为文件大小。PlanValidator绑定session/base及可信文件几何，流式验证页连续性、最后块实际字节数和DONE统计，仅保存计数及最后边界。

missing_bitmap为M3相同的小写hex编码：块索引i对应字节i/8的低位i%8，1表示仍缺失。状态正文没有文件几何，客户端必须按已绑定源元数据另行验证精确位图长度及尾部未使用位。非TRANSFERRING/VERIFYING阶段必须为空字符串。result在COMMITTED必须是完整DELTA_RESULT对象且session_id一致，其余阶段必须null。

## 阶段和持久边界

phase只允许COLLECTING、PLAN_READY、TRANSFERRING、VERIFYING、COMMITTING、COMMITTED、FAILED、EXPIRED。preview不得进入传输、验证、提交或已提交阶段。COLLECTING接受逐页清单及DONE；完全相同页重试由持久层判定幂等，不同内容报CONFLICT，跳页报MALFORMED_MESSAGE。MANIFEST_ACK仅承诺页元数据持久，不承诺数据块。

DONE清单后持久差异计划进入PLAN_READY，客户端逐页拉取。upload最后一次请求DONE时持久复制复用块并进入TRANSFERRING，preview保留PLAN_READY且不建立上传文件或确认块。DATA_CHUNK及CHUNK_ACK复用旧类型，仅在upload传输阶段有效。COMMITTING恢复期间查询返回BUSY。提交结果持久后才能发DELTA_RESULT，重复提交返回同一结果。重连的TRANSFERRING由已校验源身份及查询位图恢复；PLAN_READY必须继续拉取完整计划。

协议结构错误抛既有UploadFailure(MALFORMED_MESSAGE)；复用metadata/path验证可能抛BeginUploadError，业务层同样映射MALFORMED_MESSAGE。CONFLICT、BUSY、配额、校验等业务错误沿用既有ERROR，不新增宽泛错误码。

## 纯差异计算与资源边界

compareBlock仅在同索引、同长度、同SHA-256时复用；块大小策略由调用方在不同分块时不传入基线描述符以强制全传。此算法是固定索引比较，不是rsync。

RangePageBuilder保存至多512个已闭区间和一个开放区间。每次push后调用takePage收取已满页，finish后再takePage收尾；不及时消费会明确抛length_error，绝不累计全文件计划。索引必须严格递增且小于65536。65536交错块产生32768区间、64页；连续65536块只产生一个区间。

## 本模块验证记录

2026-11-01：Windows MinGW 13直接以-std=c++17 -Iinclude -Ithird_party编译三个专项可执行程序（build/protocol-agent），初版通过；补充分页未消费边界测试后，planner实际出现assert(caught)失败，添加finish容量检查后重跑。协议初版未先跑可编译空实现，不能将其写为完整TDD红绿证据。Linux、完整CMake、网络/存储实际传输验收由主任务统一执行；这里不推断其通过。

最终专项结果：delta_control_test、plan_page_test、delta_planner_test三个独立MinGW可执行程序均退出0；-Wall -Wextra -Wpedantic对三个新增实现的语法检查退出0，无诊断。专项测试显式取消NDEBUG以保留Release构建断言。协议golden、重复键/NUL/浮点、512/513区间、跨页相邻拒绝、空DONE、末块统计、阶段状态及65536交错块均有覆盖。


契约裁决：UPLOAD_PLAN_DONE增加必填change_kind，值仅Added/Modified/Unchanged，PlanDone末尾默认Modified以保持源码aggregate初始化兼容。服务端比较固定基线size及整文件SHA-256决定内容是否改变，不能用missing_blocks==0推断不变：删除尾块或截空可能零上传但内容已修改。客户端预览读取此明确分类。消息尚未发布，本次统一冻结新字段；缺字段或非法枚举严格拒绝。新增golden字段测试实际先assert失败，再实施字段。
