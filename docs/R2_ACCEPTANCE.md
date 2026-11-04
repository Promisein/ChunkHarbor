# ChunkHarbor R2 最终综合验收

验收日期为 2026-11-04。**R2 在现有单用户、可信本机或 SSH 隧道、Windows 本机 NTFS 的范围内达到关闭标准。** 业务闭环、兼容性和数据保护未发现阻断问题。Windows 原生 ComboBox 自动输入仍间歇失败，按 R1 已记录的例外保留，本次全量结果为 31/32。R3 等待另行指令。

## 功能验收

| 子阶段 | 本轮证据 |
| --- | --- |
| [R2.1 当前单文件](R2_1_GUI_DOWNLOAD.md) | Windows 文件列表下载当前 C。另经 SSH 下载空文件、中文名文件和多块文件，服务端与本地 SHA-256 一致。旧 ID 作为当前版本前置条件返回 `VERSION_CONFLICT`。 |
| [R2.2 历史查询](R2_2_VERSION_HISTORY.md) | GUI 查询服务端历史，旧夹具迁移及当前标记正确，服务端用一条和两条分两页读完 C/B/A。旧迁移时间保留为空。 |
| [R2.3 指定历史版本](R2_3_HISTORY_RESTORE.md) | GUI 恢复 A，协议探针分别读取 A/B/C 并逐字节核对。v2 夹具直接由当前 v4 服务端启动后，Windows 还原夹具 A/B/C。 |
| [R2.4 服务端快照](R2_4_DIRECTORY_SNAPSHOT.md) | 创建于 B，之后发布 C 并重启，旧清单以每页一条查询仍完全一致。Ubuntu 回归覆盖事务失败、提交前 SIGKILL、并发更新、引用及分页边界。 |
| [R2.5.1 快照浏览](R2_5_1_SNAPSHOT_PREVIEW.md) | Windows 读取旧快照并完成预览，专项覆盖 10,000 条、1,667 次分段请求、异常页、提前断开、取消及过期响应。 |
| [R2.5.2 旧快照目录恢复](R2_5_2_SNAPSHOT_RESTORE.md) | Windows 以已存在的 B 快照为输入，将三条映射恢复到新的 NTFS 目录。129 文件串行恢复、目标冲突、短名别名和重解析点由专项回归覆盖。 |
| [R2.5.3 当前目录下载](R2_5_3_CURRENT_FOLDER_DOWNLOAD.md) | Windows 确认创建当前快照后恢复 C。真实 SSH 在读取清单和首个数据块时分别更新远端文件，下载仍读到固定版本，兄弟前缀未混入。补测真实已提交创建应答丢失。 |

## 同一备份根上的闭环

最终运行使用 Windows 临时根 `chunkharbor-r2-flow-wwu3lltj` 和 Ubuntu `/tmp/chunkharbor-r2-flow-fK9knZaV`。测试辅助程序加载正式 GUI 库，以 Qt 非原生目录选择框选择目录，然后操作备份页的扫描与开始按钮。当前文件页、历史页和目录快照页也通过 Qt 控件驱动。这里的目录选择和确认均为自动输入。

1. Windows 以 auto 模式备份逻辑根 `audit`。文件集含 1,048,595 字节的 `large.bin`、空文件及中文多层路径 `子目录/中文.txt`。排除目录 `build` 没有进入结果。使用 GUI 支持的 64 KiB 分块，A 传 17 块，B 修改后传 1 块，创建快照后再次修改得到 C，C 的大文件也只传 1 块。中文文件在 C 更新。
2. B 提交后，通过协议创建旧快照 `cfa2069de21f9b4a3a25a3006f7bc104`。C 提交后停止并在同一端口重启服务端，再查旧快照，完整清单和版本 ID 与创建时一致。服务端 `PRAGMA integrity_check` 为 `ok`，外键检查为空。
3. Windows 文件页下载 C，版本页选 A 恢复到另一新路径。另用历史协议对 A/B/C 逐字节核对。旧快照恢复直接选择 B 的已有 ID。当前文件夹下载在确认后另建快照 `82d21c56ce637eb44a1426236c10c043`，捕获当时的 C 并恢复到另一新目录。两棵树逐路径比较，分别精确匹配 B 和 C。
4. 两种恢复前后以只读 SQLite 查询比较 `current_files` 和版本行，逐个对照六份 `.versions/<id>/data` 的大小及 SHA-256，并比较当前副本和其版本对象。版本行、当前指针及对象内容一致。两个单文件与两个目录都写入测试专用新路径。已有目标和竞争创建的保护由故障回归另行验证。

### 实际 SHA-256

| 样本 | 字节数 | SHA-256 |
| --- | ---: | --- |
| `large.bin` A，历史单文件 | 1048595 | `685f7e1c8fd5bb9a3d4ee2b1147fcb75bd87d89fa8ffb45984c22cc66d62ca37` |
| `large.bin` B，旧快照 | 1048595 | `83f81628dab5e74a0198c053c5dfddaf99a0aa705ae0b14c488dec636025fddf` |
| `large.bin` C，当前单文件和当前目录 | 1048595 | `72b20a75d775060c416a49d3161579cf3965d720933005f29c267b4dee45b4c4` |
| 旧快照 `子目录/中文.txt` | 18 | `d3bf17ef1687da36797cc7648f34c914941b77152f6f22ec258ada8a8a7027cb` |
| 当前目录 `子目录/中文.txt` | 18 | `4f58a967f3406b842e42638f1655cfbecd46cb02b15bc2e515190147af798065` |
| 两种目录的 `empty` | 0 | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| v2 夹具 A / B / C，各 14 字节 | 42 合计 | `420dbf371827b6075d7aa33200540df7bddc39ed00e5ee982486061678523c21` / `bd71325ea42140323bf1ce3b3f78b488a4b26f6c3edda9438f45ad96fc3bc468` / `7a2fb88ecfd47ffd4d45cd3aac517bf90539a7aaa5eee908028f30b6a581828f` |

## 故障、兼容性与架构核对

| 场景 | 实际证据 |
| --- | --- |
| 上传中断、取消、关窗、服务端重启 | `windows_gui_ssh_test.py` 在持久 ACK 后终止 Windows GUI 探针并 SIGKILL Ubuntu 服务端，重启后继续并核对整文件摘要。另测暂停取消、关窗续传、源变化和服务器基线冲突。`windows_mode_ssh_test.py` 的 12 组通过，含有效位图、混合 auto 策略、损坏或过期检查点隔离和不重复提交。 |
| 下载断连、取消、关窗及校验 | Windows `gui_download_test`、`gui_history_restore_test`、`gui_download_lifetime_test` 和 `snapshot_page_test` 通过。覆盖中途断开、主动取消、对象销毁、分块和整文件损坏、清单不完整及目录中途文件失败。 |
| 目标存在、竞态及清理 | Windows 通过 `download_output_test` 与目录专项，覆盖目标竞争、已有文件、路径及重解析点冲突、拒绝发布半成品、清理自身暂存和保留外来对象。盘满是错误注入。 |
| 下载中发布新版本 | SSH 当前目录专项在首个清单请求和首块读取时分别提交 D、E，Windows 仍恢复原来的 C、旧上传及中文文件。 |
| 创建提交后应答丢失 | 真实 SSH 代理接收成功的 39 帧后丢弃。收到的请求顺序为连接一 `HELLO(1), LIST(40), CREATE(37)`，连接二 `HELLO(1), LIST(40)`。GUI 显示已提交候选 `9a7548abd240152c31dbf9770ff61b02`，未选中候选、未再次创建。服务端另查候选清单逐项正确。 |
| v2→v3→v4、旧客户端和未完成会话 | Ubuntu `version_history_integration` 在真实 v2 副本上覆盖迁移失败与重启、旧元数据、当前指针、外键和查询计划。最终 schema 为 4，旧会话位图 `02` 只补 `EFGH` 后提交。为避免长期夹具过期，测试仅刷新副本的租期。另用真实 v3 副本覆盖 v4 事务回滚及重启，并在 Windows 从 v2 副本迁移出的 v4 服务端恢复 A/B/C。 |
| 协议兼容与版本对象 | HELLO 能力分离。现有 `download_v1` 约束当前版本，`historical_download_v1` 同时匹配逻辑路径和版本 ID。测试包含旧客户端、越权路径与无效 ID、畸形与超限分页、旧版本固定读取以及对象损坏检测。 |

源码核对了 `UploadHandler` 的能力路由、`SqliteStore` 的顺序迁移、`SqliteSnapshot.inc` 的事务与路径范围、`DownloadStorage.inc` 的历史对象读取，以及 Windows 下载、清单预检和目录暂存流程。快照以 SQLite 同一事务固定映射，历史下载持有不可变对象 FD。Windows 单文件和目录最终发布都由系统执行拒绝覆盖，目录复用同一个执行引擎。对现有 Git diff 和未跟踪的 R2 源码及测试进行了检查。未发现遗留调试输出或重复的目录传输实现。README 的功能摘要和页数仍停在较早阶段，路线图 R1 段的「R2尚未启动」是历史记录，当前状态以本页和 PROGRESS 为准。本轮未改生产代码或测试配置。

## 构建、测试及 GUI 证据

| 环境 | 命令与结果 |
| --- | --- |
| Windows 11 26H2，NTFS，Qt 6.8.3 / MinGW 13.1，Ninja Debug | `cmake -S . -B build-windows-gui` 配置成功，`cmake --build build-windows-gui -j 2` 成功，增量构建报告无待构建目标。设置 `QT_QPA_PLATFORM=windows` 后执行 `ctest --test-dir build-windows-gui --output-on-failure -j 2`，**31/32**，103.12 秒。唯一失败为 `combo_popup_test` 的 `mouse opens actual popup`。 |
| Windows 原生弹层专项 | 用 Windows 平台插件运行现有测试，分别设 `QT_SCALE_FACTOR=0.8,1,1.2`。实际 DPR 1.0 和 1.25 的正常及最大化用例通过。DPR 1.5 的正常用例完成，最大化设置页第二个下拉框打开失败。全量首跑在 DPR 1.25 的正常日志页也复现同一断言。QTest 输入与截图属于原生显示自动测试，物理鼠标及系统 DPI 切换未验收。 |
| Ubuntu 22.04，Linux 6.8.0-138，GCC 11.4，CMake 3.22.1 | 当前工作区源码复制到 `/tmp/chunkharbor-r2-accept-YIApmBPU`。Unix Makefiles Debug 配置、`cmake --build build-make -j 3` 和 `ctest --test-dir build-make --output-on-failure` 成功，**57/57**，41.61 秒。首次 Ninja 配置因未安装 Ninja 失败，改用 Makefiles 并补齐本机 OpenSSL/SQLite 开发路径及 OpenSSL 多架构头路径后通过。 |
| Windows→SSH→Ubuntu | 同根端到端闭环、R1 崩溃续传和模式矩阵、当前单文件、v2 夹具历史恢复、当前目录版本变化均退出 0。只使用测试临时根，未触及实际备份数据。 |

Windows 全量日志为 `build-windows-gui/r2-accept-ctest.log`，配置与构建为 `r2-accept-config.log` 和 `r2-accept-build.log`。同一构建目录下的 `r2-acceptance/` 保存 `flow-final.log`、`acceptance_flow.py`、临时 GUI 探针源码、`r1-ssh.log`、`r1-mode-ssh.log`、`migration-ssh.log`、`current-single-ssh.log`、`current-race-ssh.log` 和三档原生弹层日志。Ubuntu 隔离源码根保存 `config-final.log`、`config-with-headers.log`、`build-final.log` 与 `ctest-final.log`。

```powershell
$src = '/tmp/chunkharbor-r2-accept-YIApmBPU'
$server = "$src/build-make/chunkharbor-server"
python build-windows-gui/r2-acceptance/acceptance_flow.py miniob-ubuntu $server --gui-buttons --real-loss
python tests/integration/windows_gui_ssh_test.py build-windows-gui/chunkharbor-gui-transfer-probe.exe miniob-ubuntu $server
python tests/integration/windows_gui_download_ssh_test.py build-windows-gui/chunkharbor-gui-download-probe.exe miniob-ubuntu $server
python tests/integration/windows_gui_history_ssh_test.py build-windows-gui/chunkharbor-gui-download-probe.exe miniob-ubuntu $server "$src/tests/fixtures/r22-v2-backup.tar.gz" --restore
python tests/integration/windows_gui_history_ssh_test.py build-windows-gui/chunkharbor-snapshot-page-test.exe miniob-ubuntu $server "$src/tests/fixtures/r24-v3-backup.tar.gz" --current-folder
```

## 风险分级及 R3 前基线

| 级别 | 结论 |
| --- | --- |
| A 阻止关闭 | 未发现。已验证窗口范围内的备份、断点续传、版本一致性、读取和安全发布。 |
| B 已知限制 | 单文件 64 MiB。当前与历史单文件共用一个单文件任务，目录任务独立且内部串行，两者之间没有全局互斥。下载无续传。独立空目录和源端删除标记未入库。当前文件夹下载会创建持久快照。协议缺少请求级幂等 ID，查询结果仅为候选。整目录发布限于 Windows 本机固定 NTFS，单文件与目录都保留现有路径约束。同属主主动篡改和掉电耐久性不在本轮保证内。R1 的 ComboBox 自动输入问题仍间歇复现。 |
| C 后续规划 | R3 保留 64 MiB 边界基线，另行设计 GB 文件、在途容量、真实盘满、资源测量及下载续传取舍。R4 处理版本与持久快照的保留、回收、长期运行及崩溃一致性。幂等创建、跨平台发布和规模性能需要单独设计与验证。 |

仍未验证硬件掉电、真实磁盘耗尽、MSVC、人工原生窗口与原生选择框、系统 DPI 切换或混合 DPI 跨屏，以及远端万文件完整恢复。现有万项清单测试和 129 文件本地对端恢复覆盖了两个不同规模边界。

进入 R3 前保留 v2、v3 两份真实夹具及其 SHA-256，现有 HELLO 能力及当前版本前置条件，64 MiB / 256 KiB 下载边界，快照完整页约束，六对象闭环和真实 SSH 并发/丢应答探针，以及失败清理和无覆盖测试。留存本轮全量原生失败日志和独立 DPI 结果，避免回归时把间歇失败误记为全面通过。
