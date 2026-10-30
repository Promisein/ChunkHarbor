# M1 验收记录（2026-10-30）

## 验收映射

| 要求 | 实现 / 自动验证 |
|---|---|
| 单loop LT Reactor | EventLoop/EpollPoller/Channel；eventloop_test、wakeup_test |
| 生命周期 | tie弱引用、派发强引用、移除后停止派发；channel_lifetime_test |
| 有界流式Buffer | buffer_test，5000次参考模型操作 |
| 非阻塞读写与错误 | tcp_connection_test使用socketpair、小SO_SNDBUF、链接器包装注入send/recv EINTR与短写，断言发生真实EAGAIN和完整排空 |
| eventfd/epoll EINTR | wakeup_test注入write/read/epoll_wait的EINTR并验证恢复；队列上限拒绝、quit-before-run |
| 停止与资源 | server_stop_test验证stop后可立即在同一端口重建server；集成测试SIGTERM正常退出 |
| TCP半包/粘包/半关闭 | network_integration分段帧头、连续帧、空帧、64KiB/1MiB、shutdown(SHUT_WR)后响应完整 |
| 畸形输入 | magic/version/type/flags/reserved、超限/UINT64_MAX长度、截断帧、未实现业务编号 |
| 并发与隔离 | 100个已连接客户端经barrier同时发送各自payload/request_id，逐字节验证 |
| 慢读与FD回收 | 慢客户端暂不读时健康客户端仍收到响应；队列限额单测、RSS安全上限、200次快速FIN/RST后/proc FD数恢复 |
| 空闲与连接数 | timerfd过期关闭；max-connections=1拒绝额外连接 |
| 客户端期限 | fake loopback peer接收后沉默/只回CHKH，CLI约5秒非零退出 |
| 跨平台协议 | Windows MinGW与Ubuntu运行同一显式35字节golden vector；逐个分割点验证NeedMore |

## 实际命令和结果

Ubuntu正式目录 `/home/ning/ChunkHarbor`，CMake3.22.1、GCC11.4、Python3.10.12。Debug、Release、ASan+UBSan均构建并通过 **11/11** CTest。没有使用Ninja，因为虚拟机没有安装；没有安装Qt。

```bash
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 2
ctest --test-dir build --output-on-failure
cmake -S . -B build-release -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j 2
ctest --test-dir build-release --output-on-failure
cmake -S . -B build-asan -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCHUNKHARBOR_SANITIZERS=ON
cmake --build build-asan -j 2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-asan --output-on-failure
```

Sanitizer启用非PIE链接以避免虚拟化环境中的ASan地址映射冲突，覆盖本轮全部Linux测试；未执行TSan。Windows实测如下（路径是本机现有工具）：

```powershell
cmake -S . -B build-windows-protocol -G Ninja '-DCMAKE_MAKE_PROGRAM=D:/CLion 2023.3.6/bin/ninja/win/x64/ninja.exe' -DCMAKE_CXX_COMPILER=C:/mingw/MinGW/bin/g++.exe -DCMAKE_BUILD_TYPE=Debug -DCHUNKHARBOR_BUILD_SERVER=OFF -DCHUNKHARBOR_BUILD_CLI=OFF -DCHUNKHARBOR_BUILD_GUI=OFF
cmake --build build-windows-protocol -j 2
ctest --test-dir build-windows-protocol --output-on-failure
```

Windows构建protocol/client_core成功，测试 **1/1通过**；这不是Qt GUI验证。Windows/Linux协议golden vector相同，无Qt、POSIX或Linux net依赖。

## 审查和限制

独立审查未发现阻断问题；提出的stop保留监听FD、CLI缺少异常响应自动测试建议均已处理。回归先复现stop占用端口，再修复并通过全套测试。Channel/FrameCodec/TCP测试在实现前曾因缺少对应接口而失败，随后转为通过。

以上仅证明测试场景内的功能与资源释放；未进行性能评估、不提供QPS或吞吐量。内核socket缓冲与编码/扩容副本不计入Buffer逻辑上限。真实不可信网络部署仍需M7安全能力。M2文件上传与M3崩溃一致性均未开始。
