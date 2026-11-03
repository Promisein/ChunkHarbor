# R1：上传 server disconnected 与磁盘工作期间的空闲回收（2026-11-03）

> 阶段收尾补充：用户明确要求不再追查约10.9MiB照片旧截图的断连，并关闭R1。该现场报告已从阶段待办移除；网络波动是可能解释，不是新确认的根因。已证实的服务端空闲回收缺陷修复及恢复测试继续有效。以下原始分析、失败与验证边界保留，后续实际新问题按新证据处理；最新状态见 [PROGRESS](PROGRESS.md)。

## 补充：约10.9MiB与默认30秒期限的隔离验证

用户补充：失败文件为中文路径JPEG、约10.9MiB，其他文件继续，无手工暂停的直接证据，Windows经SSH连接Ubuntu；仍无完整服务端日志/等待时长。该大小低于64MiB限制，不应按GB大文件或手工暂停归因。本补测沿用上轮生产修复，没有更改生产超时/状态机/健康检查或关闭安全保护。

新增 `tests/integration/windows_upload_disconnect_matrix.py`，仅在自建测试根使用11,429,478字节随机合成文件及64KiB后续文件，不读取用户照片。沿用真实GUI probe（32KiB分块，共349块），让前8块取得真实ACK，再在第9块持久化前注入。测试hook新增有界参数CH_UPLOAD_DELAY_MS（0–45000，缺省2000）、CH_UPLOAD_DELAY_SKIP（0–65536，缺省0），原CTest注入语义不变。server命令省略idle-timeout-ms，使用实际默认30秒；客户端30秒请求期限未改。

| 模式 | 注入与实际结果 | 失败后恢复证据 |
| --- | --- | --- |
| normal | 不延迟；11,429,478字节上传成功，整批8.47秒 | 两文件SHA-256一致 |
| slow | 第9块同步前等待28秒；成功，整批36.58秒；marker和耗时下界均确认注入生效 | 无伪超时，两文件哈希一致 |
| timeout | 第9块同步前等待32秒；首文件原始错误为server response timed out，后续文件成功；整批33.52秒 | checkpoint仍未完成；服务端后来持久9块，恢复仅补340块，最终两文件哈希一致 |
| tunnel | marker出现后仅终止本脚本SSH转发；另路SSH确认server仍在；首文件server disconnected，后续文件连接也失败 | 重建隧道、查询保留9块，仅补340块；两文件哈希一致 |
| server | marker出现后校验PID/exe再SIGKILL专用server；SSH转发保留；首文件server disconnected，后续文件也失败 | 原根/原端口重启、不注入；启动恢复保留前8块，仅补341块，两文件哈希一致 |

因此客户端期限是**单次请求从发送到响应的等待**（也包含发送排空），不是整个文件或批次期限；28秒同步+其他处理使整批超过30秒仍可成功。32秒例子证实修复后慢操作不会被server网络空闲回收，却仍受client请求期限限制；不能把这个客户端timeout例子重新当作server主动idle关闭的证据。此前旧代码缩短idle的EOF红测仍独立有效，但不足以认定原截图就是默认30秒的同一竞态。

恢复时客户端没收到第9块ACK，服务端仍可能完成该块；测试使用只读SQLite检查 accepted 块，再由真实GUI执行QUERY_DELTA/Resume，验证实际发送块数，不能假设“无ACK一定未写入”。每文件try/catch+独立Peer符合当前部分失败继续处理设计：只有单文件或短暂失败时后续可能成功；若整个SSH或server持续失效，后续也会失败。原截图的后续成功与持续性整条隧道中断不太吻合，但不能据此排除瞬时网络/转发channel或单TCP连接关闭。

实际执行：

```bash
# Ubuntu原工程，仅重编测试hook
cmake --build build --target chunkharbor-upload-delay-hook -j 2
ctest --test-dir build -R 'application_pause_test|tcp_connection_test|upload_idle_integration|delta_fault_integration|resume_integration' --output-on-failure
```

构建退出0，关联CTest **5/5通过**（6.02秒）；没有改生产文件，也未重复整个Linux51项。Windows用已验证GUI二进制逐模式实际执行下述命令（mode为normal/slow/timeout/tunnel/server）：

```powershell
python -X utf8 tests/integration/windows_upload_disconnect_matrix.py build-windows-gui/chunkharbor-gui-transfer-probe.exe miniob-ubuntu /home/ning/ChunkHarbor/build/chunkharbor-server /home/ning/ChunkHarbor/build/libchunkharbor-upload-delay-hook.so MODE
```

五种模式均退出0。恢复块数断言增加后，timeout/tunnel/server再次通过；独立审查建议核验slow注入生效及修正异常清理，已补marker+耗时下界、保证server清理失败时仍终止隧道并保留原异常，slow和normal复核通过。没有将测试脚本的总耗时限制当作产品单请求期限。未再次构建Windows或复跑23项GUI全量（生产GUI/协议均未改）；这些仍是上轮结果。

最终脚本（含marker和清理改动）再次执行timeout/tunnel/server均通过：timeout整批33.83秒、保留9块补340块，根 `/tmp/chunkharbor-disconnect-matrix-mFl2XDuF`；tunnel根 `/tmp/chunkharbor-disconnect-matrix-0VD9jyJA`，保留9块补340块；server根 `/tmp/chunkharbor-disconnect-matrix-55oxSvnF`，保留8块补341块。三者最终两文件哈希均一致。未对清理时SSH自身失效的异常路径再做专门故障注入。

专用根：normal `/tmp/chunkharbor-disconnect-matrix-Fy0eSbNG`，slow `...-0w4gddKf`，timeout `...-O7jteQ2c`，tunnel `...-iTmeTEBp`，server `...-63ZsnAmm`（后三者等省略号均为同一/tmp/chunkharbor-disconnect-matrix前缀）。服务、GUI和自建隧道已结束，测试根保留；没有影响用户隧道/数据，未提交/push。

**还不能归因原截图**：原始server disconnected只证明该Peer观察到TCP结束，无法独自区分server的idle/协议拒绝/资源错误、SSH转发channel结束或对端退出。尚缺客户端错误时间、请求类型/ID/等待时长、QTcpSocket错误明细，服务端同一连接关闭原因/idle设置/进程退出信息，以及同时间的SSH转发日志和实际二进制版本。当前GUI持久日志不包含全部这些字段；需在下一次真实复现时采集/针对性补诊断，不能凭照片大小判断根因。本轮不增加无证据的自动重试或放宽期限。

未测试物理链路丢包/黑洞、真实硬件磁盘长尾/掉电、原照片及原运行负载；受控注入不是这些场景的替代证据。未新增生产代码修复；R1只继续本上传断连核验，到此停止。

## 结论与证据边界

确认并修复一条可重复根因：服务端接受上传请求后暂停读取并交给有界磁盘工作池，但 TcpServer 的定时回收只检查最近网络收发时间。排队、校验或持久化跨过空闲阈值时，连接会在结果返回前被关闭，Windows独立上传Peer报 `server disconnected`。

这不是Qt健康检查断开：报错位于 BackupWorker.cpp 的 `Peer::request()`；健康探测由另一个 QtTcpTransport 管理。上传每文件在try/catch内创建独立Peer，失败清空该文件versionId、记录错误并保留checkpoint，随后循环继续，为下一文件重新连接。因此一个文件失败不代表后续都失败；若工作池/数据库仍堵塞，后续文件也可能失败。

没有取得用户原始故障的日志/时长/部署参数。本轮使用缩短空闲阈值和受控慢磁盘重现，证明存在该缺陷，不能断言用户历史所有断开都只有这一原因。默认空闲30秒、客户端响应30秒未上调；真实SSH断线、暂停过久、本地哈希长时间不发包、真实磁盘故障仍可能产生其他中断。

## 重现过程

1. 新增仅测试使用的LD_PRELOAD库，在`.delta`文件首次fdatasync之前创建独占marker并等待2秒，随后才执行真实fdatasync。产品二进制不链接此库。
2. Linux真实服务端idle=500ms：发送块后进入上述边界，旧代码在fdatasync完成前返回EOF，测试报 `pending disk work was closed or acknowledged early: b''`。不允许提前ACK来绕过等待。
3. Windows真实GUI/BackupWorker→SSH→Ubuntu：idle=1500ms、两个随机64KiB文件。旧server的首文件报出原文server disconnected且version为空，第二文件使用独立Peer提交成功；再用相同客户端状态Resume，两个最终文件SHA-256一致。
4. 同一GUI脚本不带expect-disconnect运行修复后的server，慢首文件与后续文件均成功且哈希一致。没有修改BackupWorker、独立Peer或QtTcpTransport来吞掉异常。

测试失败记录：最初GUI重现使用500ms阈值，第一慢操作还持有数据库锁时第二请求也等超时，实际两文件失败，与脚本期望“一失败一成功”不符。改用1500ms隔离首文件超时与后续恢复，不改生产默认值。首轮修复后Linux测试误用ACK字段length，真实协议是data_length，导致KeyError；修正夹具后通过。这些失败均未算通过。

## 最小实现与所有权

- `TcpConnection::idleTimeoutSuspended()`：只在 `applicationPaused_ && !readEof_` 时为true。它表达服务端自己暂停读取等待业务工作，不是客户端暂停按钮或TCP发送背压。
- `TcpServer::timerReady()`：跳过上述等待中的连接；普通连接照常按lastActivity回收。
- UploadHandler原逻辑不变：pauseReading→trySubmit；满队列返回BUSY并关闭；工作完成回所属EventLoop，取得弱连接的强引用、检查closed后发结果，随后queueInLoop恢复读取。错误走shutdownAfterWrite，其readEof使回收不再暂停。回调不能在已释放连接上发送。
- 成功发送仍更新lastActivity，恢复读取后继续普通空闲策略。磁盘池仍2线程、64个未完成任务，连接数仍受原限制；没有修改队列/并发上界、协议、schema或持久确认语义。

本轮仅改两个生产文件：include/chunkharbor/net/TcpConnection.h、src/net/TcpServer.cpp。测试另改application_pause_test/tcp_connection_test及CMake，新增upload_delay_hook、upload_idle_test和windows_upload_idle_ssh_test。

## 数据安全与测试覆盖

upload_idle_integration在真实fdatasync之前跨过空闲阈值，断言没有ACK也没有EOF；完成后才验证durable ACK、补第二块、COMMIT与文件内容。另验证普通无请求连接仍被主动回收、重新连接正常上传。最后在慢写期间SO_LINGER=0制造TCP RST并丢失ACK，等待工作完成后重连QUERY_DELTA，服务端missing_bitmap=02，仅补第二块并提交，最终内容一致。不从“已发送”推断“已持久化”，也不以断连推断服务端一定没写成功。

application_pause_test检查业务暂停/恢复及真正有待发送输出的shutdownAfterWrite；tcp_connection_test检查慢端高水位/EAGAIN不获得空闲豁免。独立审查发现原排空夹具立即关闭不能证明待排空状态，已改为小SO_SNDBUF加512KiB输出，并断言连接仍开但回收未暂停。

## 实际平台、命令与结果

Ubuntu22.04原工程 `/home/ning/ChunkHarbor`，GUI=OFF，复用原SQLite/OpenSSL缓存。同步前远端干净，本轮仅复制上述Linux修改、CMake和Linux测试；没有同步此前Qt健康源码或所有文档，不声称三端工作区完全一致。本轮网络生命周期有变，实际完整构建恢复了此前清理的测试产物，以便全量回归；未清理用户备份或操作常驻服务。

```bash
cmake -S . -B build
cmake --build build -j 2
ctest --test-dir build --output-on-failure
```

配置/构建成功，最终生产代码的全量 **51/51通过**（36.72秒，不是性能指标）；含上传、续传、delta故障注入、网络、磁盘池及新增上传idle专项。此后仅补测试断言和RST场景，专项通过，未改生产代码。

Windows Qt6.8.3/MinGW13.1/Ninja Debug：

```powershell
$env:PATH='D:/QT/Tools/mingw1310_64/bin;D:/QT/6.8.3/mingw_64/bin;'+$env:PATH
cmake -S . -B build-windows-gui
cmake --build build-windows-gui -j 2
ctest --test-dir build-windows-gui --output-on-failure
python -X utf8 tests/integration/windows_upload_idle_ssh_test.py build-windows-gui/chunkharbor-gui-transfer-probe.exe miniob-ubuntu /home/ning/ChunkHarbor/build/chunkharbor-server /home/ning/ChunkHarbor/build/libchunkharbor-upload-delay-hook.so
python -X utf8 tests/integration/windows_gui_ssh_test.py build-windows-gui/chunkharbor-gui-transfer-probe.exe miniob-ubuntu /home/ning/ChunkHarbor/build/chunkharbor-server
```

配置/构建成功、完整 **23/23通过**（117.35秒）；慢持久化GUI脚本通过，最后复核临时根 `/tmp/chunkharbor-upload-idle-gui-2U9a8RSL`。旧server复现额外带 `--expect-disconnect`，成功复现并安全续传，根 `/tmp/chunkharbor-upload-idle-gui-te0NObDs`。

真实GUI全流程通过：首次/空文件/排除/中文/0块、单块变化、源与基线保护（仅回归既有行为）、GUI进程kill+server SIGKILL后重开续传、暂停取消与Resume、协作关窗重开，均核验SHA-256。最终收紧排空判定后再次全流程通过，专用根 `/tmp/chunkharbor-m5-data-o5YdxVuX`，未作为新模式开发。测试服务和隧道已停止，专用根保留，未触碰用户真实备份。

## 剩余问题与知识点

空闲超时针对“对端无活动”，不能误当“服务端尚未处理完”；背压、业务暂停、半关闭是不同状态。TCP断开时提交结果可能不确定，必须查询持久会话，不能盲重发新BEGIN或伪造成功。RAII释放Peer的socket，shared/weak_ptr保证跨线程完成回调引用有效，仍不能代替磁盘/SQLite恢复协议。

没有引入服务端工作硬截止/可取消磁盘系统调用：永久卡死的磁盘工作仍可能占用有界任务与连接，客户端仍可能30秒超时；相关超时/运维归后续R1复现或R3，不因本次修复宣称磁盘永不阻塞。客户端暂停/哈希期间的真正空闲仍会被回收，是否改进需独立复现，不在本轮猜测修改。没有新增自动重连策略，沿用安全手工重试/恢复。

未执行Release、ASan/UBSan/TSan、MSVC、Linux GUI、默认30秒阈值的长期慢磁盘试验、物理掉线/真实磁盘满/硬件掉电、GB文件或性能测试。RST与慢fdatasync是受控故障模拟。未提交/推送，R1其他任务不自动开始。
