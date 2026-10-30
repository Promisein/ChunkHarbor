# ChunkHarbor

C++17 单向增量备份项目，规划为 Linux Reactor 服务端、Linux CLI 与 Windows Qt GUI。**当前完成 M1 网络与帧协议：可运行有界、非阻塞的 TCP Echo；文件备份属于 M2，尚未实现。**

## Ubuntu 构建与验证

正式开发目录为 `/home/ning/ChunkHarbor`，无需 Qt。依赖 CMake 3.20+、GCC、Make、Python 3；GDB 为可选调试工具。

```bash
ssh miniob-ubuntu
cd /home/ning/ChunkHarbor
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 2
ctest --test-dir build --output-on-failure
```

当前有 11 项 CTest：基础资源、Buffer、事件循环、Channel 生命周期、TCP 短写/EAGAIN/EINTR、eventfd 中断、停止后端口释放、帧协议、两个 help 和网络集成。集成测试默认 100 个同时连接，可用 `CHUNKHARBOR_TEST_CLIENTS=200 ctest --test-dir build -R network_integration -V` 调整（上限受服务器连接配置限制）。这些是功能测试，不是吞吐量指标。

## 运行 Echo

Ubuntu 终端一：

```bash
./build/chunkharbor-server --echo --port 8787
```

看到 `LISTENING 127.0.0.1:8787` 后，在另一个 Ubuntu 终端执行：

```bash
cd /home/ning/ChunkHarbor
./build/chunkharbor-cli echo --host 127.0.0.1 --port 8787 --message "hello ChunkHarbor"
```

期望：`ECHO OK: hello ChunkHarbor`。服务端 Ctrl+C 或 SIGTERM 正常关闭；支持 `--idle-timeout-ms 30000`、`--max-connections 512`。`--port 0` 由系统分配端口并打印，供自动测试使用。

Echo 是独立测试模式：PING(16) 回 PONG(17)，DATA_CHUNK(5) 原样回显以测试大二进制帧；这些 payload 暂作不透明测试字节，不执行 JSON/文件块业务。其他合法但未实现的消息、非法头或超限长度会关闭连接；没有 HELLO/上传/认证语义。配置 JSON 仍只是样例，程序目前通过上述参数配置。

服务端只绑定 loopback；Windows 想访问可使用 `ssh -N -L 18787:127.0.0.1:8787 miniob-ubuntu` 建立隧道，然后用实现相同帧协议的测试客户端连接本机 18787。Qt GUI 尚不存在，普通文本客户端不能替代帧客户端。

## 自己调试

```bash
ctest --test-dir build -R network_integration -V
ctest --test-dir build -R '^buffer_test$' -V
gdb --args ./build/chunkharbor-server --echo --port 8787
```

GDB 中可设 `break chunkharbor::TcpConnection::readReady`、`break chunkharbor::TcpConnection::writeReady`、`break chunkharbor::protocol::decode`，输入 `run` 后从另一终端运行 CLI。`bt` 看调用栈，`print *this` 看连接/缓冲状态，`next` 单步，`continue` 继续。测试二进制也可直接用 GDB 启动；连接默认有空闲超时，断点停留很久时可把 `--idle-timeout-ms` 调大。

```bash
cmake -S . -B build-asan -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCHUNKHARBOR_SANITIZERS=ON
cmake --build build-asan -j 2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-asan --output-on-failure
```

## Windows 共享协议测试

使用兼容 C++17 的编译器和 Ninja（加入 PATH），无需 Qt。Windows 默认关闭 Linux server/CLI；GUI=ON 会明确报尚未实现。以下只构建纯 C++协议与 Echo 编排：

```powershell
cmake -S . -B build-windows-protocol -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCHUNKHARBOR_BUILD_SERVER=OFF -DCHUNKHARBOR_BUILD_CLI=OFF -DCHUNKHARBOR_BUILD_GUI=OFF
cmake --build build-windows-protocol
ctest --test-dir build-windows-protocol --output-on-failure
```

本机 MinGW 13.2 运行了同一 golden vector 测试；GUI、MSVC 与 Windows/Linux 实际 GUI 联调尚未本地验证。

## 实现边界

已拆分 `chunkharbor_protocol`、最小 `chunkharbor_client_core`、`chunkharbor_linux_net`。网络采用单线程 LT；多线程 Reactor 为后续可选扩展。每连接输入上限 1 MiB+32，输出上限 4 MiB，高/低水位 1 MiB/512 KiB；积压暂停读，降水位恢复读；每次事件收发有预算，连接数与空闲期有上限。容量限额不包括内核 socket 内存及短暂编码/扩容副本。

未实现：文件上传、SHA-256、SQLite、重启续传、增量备份、Qt GUI、TLS/认证。完整规格见 [指南](CODEX_DEVELOPMENT_GUIDE.md)，当前结果见 [进度](docs/PROGRESS.md)、[验收证据](docs/M1_VALIDATION.md)。
