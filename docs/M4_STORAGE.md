# M4 存储实现与验证边界

存储与旧 UploadStorage 共享根目录 flock、目录 FD、目标占用集合及互斥锁。DeltaStorage 的同步 API 只能由磁盘工作池或监听前启动阶段调用。当前实现由 UploadStorage.cpp 包含 DeltaStorage.inc，以确保既有内部 FD 和安全路径操作只有一套所有权；DeltaStorage.cpp 保留构建入口。

SQLite 从 schema v1 事务迁移到 v2，保留 transfers/transfer_chunks，增加版本、当前指针、旧会话映射、持久清单、计划和增量会话。CAS API 要求调用方显式开启事务。新库不能交给旧二进制使用。升级需要同时部署 schema_v1.sql/schema_v2.sql；此轮测试仅使用新建临时目录，不自动迁移真实用户数据。

不可变版本位于 `.versions/<random-id>/data`；M3 历史导入沿用唯一 session ID。最新副本是 files 下独立文件，不使用硬链接。旧会话校验其历史对象，避免最新内容更新使旧会话无法启动。COMMITTED/EXPIRED 暂存目录可在 meta 已清除后继续收尾。

上传清单逐页事务保存，重复页必须完全相同。finishManifest 固定基线并构造持久区间；比较调用可移植 compareBlock。预览不创建 data、不标记 accepted、不占目标写入锁。上传获取计划 DONE 时重算并复制复用块，文件同步后事务确认复用进度。收到的数据必须同时匹配清单摘要和数据摘要，fdatasync 后才持久确认。启动及 query 重验已确认块。

提交校验完整摘要、当前 version ID 及最新副本真实内容后，先持久不可变对象，再记录 COMMITTING 意图，最后原子发布和目录 fsync、事务更新当前指针与结果。启动重放 COMMITTING：最新副本匹配新摘要时补数据库，否则必须匹配旧基线后才可重新发布。A→B→A 保留不同版本身份；完整内容未变复用当前版本身份。历史内部版本目前不自动 GC，总占用会随版本增长。

预览COLLECTING活跃会话最多8个，完成计划后释放活跃名额、保留可重拉计划；总preview最多20000个、逻辑元数据64MiB、24小时过期。上传与M3合计最多32个及512MiB预留数据、30天。启动及begin清理过期资源。单文件仍限制64MiB/65536块；逻辑配额不是SQLite物理文件大小上限。

专项 tests/unit/delta_storage_test.cpp覆盖全传、零数据、单块、A→B→A、预览隔离、页幂等冲突、未完成提交拒绝及重开续传。最终统一测试另覆盖四个精确SIGKILL发布屏障、ENOSPC/同步/SQLite错误，见M4_VALIDATION。未验证物理掉电或真实磁盘满，也未穷举所有失败排列。

2026-11-01 本轮实跑：Windows `python tests/unit/schema_v2_test.py` 通过。隔离 Ubuntu `/tmp/chunkharbor-m4-storage-1101` 使用 g++17、私有 SQLite/OpenSSL 开发头及已有 nlohmann 头，显式链接 `libsqlite3.so.0`/`libcrypto.so.3` 与 `-Wl,--wrap=renameat` 编译运行 delta_storage_test，退出码 0。该专项包含在最新副本 rename 成功屏障处对子进程发送 SIGKILL 后的重开恢复；这代表真实进程崩溃验证，不代表物理掉电。尚未穷举所有发布与清理阶段的故障注入。

后续审阅修正：预览只有 COLLECTING 阶段占 8 个活跃名额，PLAN_READY 可以重拉计划而不占活跃名额；未过期预览最多 20000 个，并以请求/清单JSON长度加每块128字节、每区间64字节及行常数估算保守64MiB逻辑配额。这不是 SQLite 物理文件大小上限。24小时到期清理清单、计划、确认记录和preview会话；清理中断会在下次启动继续。上传配额也由旧上传入口统计增量会话，避免两套入口突破上限。

启动会将没有 versions 记录引用的 `.versions/<ID>` 对象安全移动到 `.quarantine/version-<ID>` 并同步目录，不删除它。发布临时副本在受限 `.delta/.publish-<ID>` 内创建后跨目录原子rename至files，避免用户合法文件名 `.publish-<ID>` 与内部暂存名称冲突；不靠禁止某类用户路径掩盖问题。专项新增连续12个预览、孤立版本隔离及暂存名碰撞回归，最终执行结果由统一CTest记录。
