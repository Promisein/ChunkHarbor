# R1：GUI 服务器可达性检查（2026-11-03）

仅处理状态误判与健康检查，不扩展上传模式、恢复或其他 R 阶段业务。

## 原流程与根因

点击检查 → TCP 建连 → HELLO/HELLO_ACK → ready → 顶部和总览显示在线。旧代码随后把探测 socket 的 disconnected 转为 statusChanged，页面对任何 statusChanged 都改成离线，因此健康服务关闭探测连接也显示离线。普通启动未检查也直接显示离线。另一方面，旧实现没有周期复查，握手成功不能证明持续健康。

gui_health_test 在修改生产代码前实际失败：`healthy HELLO followed by peer close must remain reachable, not offline`。使用真实 MainWindow、QTcpServer/QTcpSocket、合法帧和Qt事件循环，不是直接调用 ready/failed。

## 最小设计与异常路径

- QtTcpTransport 增加 `HealthState { Unchecked, Checking, Reachable, Unreachable }`、`healthState()`、`healthStateText()`、`healthChanged()`；页面显示未检查/检查中/可达/不可达，statusChanged 仅提供诊断。
- 首次由用户触发，普通启动不自动联网。每次检查独立短连接，只发既有 HELLO、严格校验 ACK；成功先标记 terminal 再 abort，资源清理不推翻结果，不要求新增 PING/PONG。
- 探测模式用 single-shot QTimer：一次成功/失败结束15秒后复查，单次仍最多5秒；复查开始即失效旧绿色结果。完成后才安排下一次，单实例最多一个在途探测。
- 手工检查立即取消旧 socket/超时/复查；disconnectFromServer 停止两种计时器，清空结果。端点修改后须手工检查新端点，不继承旧结果。
- connectForRequests 保持列表请求模式：握手后保留 socket、响应10秒超时，不启动健康复查。BackupWorker 继续独占其线程/socket；顶部结果不作为上传/列表许可条件。
- MainWindow 和 DashboardPage 订阅同一健康状态；socket和计时器均在所属Qt线程使用，连接均带QObject context，销毁时取消回调、定时器和socket。

拒绝、超时、畸形ACK、握手前断连进入不可达并保留错误详情，15秒后重试。可达只代表最近HELLO成功，不保证磁盘可写、备份能力或提交持久性；handshake模式可达也不能上传。正常事件循环下，失效结果约在下次15秒复查加最多5秒等待后更新；睡眠或事件循环停顿会延后，不能称实时探测。

## C++ / Qt 知识点

TCP建连、应用HELLO、CHUNK_ACK/COMMIT_RESULT是不同保证。`enum class` 将状态与诊断文字解耦，避免页面根据字符串和信号顺序猜测结果。同线程普通Qt信号通常直接执行槽，abort可能触发disconnected，故先设置terminal再关闭。QTimer依赖事件循环而非后台线程，完成后调度避免慢请求重叠；QObject寿命管理回调失效。

## 实际验证

Windows Qt6.8.3 / MinGW13.1 / Ninja / Debug，复用已有缓存：

```powershell
$env:PATH='D:/QT/Tools/mingw1310_64/bin;D:/QT/6.8.3/mingw_64/bin;'+$env:PATH
cmake -S . -B build-windows-gui
cmake --build build-windows-gui --target chunkharbor-gui-health-test -j 2
ctest --test-dir build-windows-gui -R '^gui_health_test$' --output-on-failure
cmake --build build-windows-gui -j 2
ctest --test-dir build-windows-gui --output-on-failure
```

健康回归：初始/检查中状态、合法ACK后对端关闭、真实15秒周期复查、TCP接受但应用沉默、5秒超时、自动恢复、检查中切端点取消旧超时、拒绝端口与手工重试、检查中销毁窗口。GUI pulse只证明事件循环运行，不是性能指标。原gui_probe七类非法输入/超时及gui_files独立请求回归保留；gui_pages移除了直接发射ready/failed冒充网络状态的断言。最终全量结果见PROGRESS顶部。

独立纯C++配置使用原MinGW13.2（不混用Qt ABI）：

```powershell
$env:PATH='C:/mingw/MinGW/bin;'+$env:PATH
cmake -S . -B build-windows-protocol
cmake --build build-windows-protocol -j 2
ctest --test-dir build-windows-protocol --output-on-failure
```

配置/构建退出0，**16/16通过**，不依赖Qt。

真实SSH冒烟使用Python subprocess编排：`ssh -o BatchMode=yes -o ConnectTimeout=8 miniob-ubuntu 'echo $$; exec /home/ning/ChunkHarbor/build/chunkharbor-server --handshake --port 0'`，读取本次PID/实际端口45563；本机临时端口通过 `ssh -o BatchMode=yes -o ExitOnForwardFailure=yes -N -L <临时端口>:127.0.0.1:45563 miniob-ubuntu` 转发，再执行 `build-windows-gui/chunkharbor-gui.exe --probe-port <临时端口>`（offscreen），退出0。结束前核对 `/proc/<PID>/exe` 等于server绝对路径后仅SIGTERM该测试进程，停止自建隧道；没有存储根和用户服务重启。这是HELLO兼容验证，不是跨机周期故障测试。

失败记录：先确认旧实现的上述行为红测；扩展测试首次报告condition timed out，补充阶段诊断后定位拒绝端口。原2秒等待短于应用5秒截止和Windows网络栈报告时间，改为7秒测试容差，生产超时未放宽；两次失败未计成功。Vulkan头与dxcompiler/dxil缺失为既有非阻断告警，不代表正式打包验收。

未执行Linux GUI/MSVC/Release/Sanitizer/长期运行、真实SSH断网/崩溃的周期恢复或新的真实GUI上传。没有修改后端/协议/schema，不将旧Linux50/50算本轮通过；Ubuntu未同步本轮源码，未重建此前清理的全套测试产物。未提交/推送。
