# ChunkHarbor

C++17 单向增量备份项目。**M1/M2已完成：Linux CLI可通过自主Reactor服务器完成分块校验、持久块回执、整文件校验和不覆盖发布；Windows Qt提供最小连接/握手原型。** GUI上传、SQLite重启续传和增量备份未实现。当前单文件上限64MiB，仅用于可信loopback/SSH开发。

## Ubuntu 构建与验证

在 Ubuntu 的仓库根目录构建，无需 Qt。依赖 CMake 3.20+、GCC、Make、Python 3、OpenSSL 开发包（Ubuntu `libssl-dev`）；GDB 为可选调试工具。

```bash
cd /path/to/ChunkHarbor
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 2
ctest --test-dir build --output-on-failure
```

当前Ubuntu有26项CTest，包含M1回归、协议、暂存/持久回执/安全发布、容量1的异步背压、真实CLI上传及恶意响应测试。Debug、ASan/UBSan、Release均通过；Windows纯协议7项、含hash8项通过。完整证据与边界见 [M2验收](docs/M2_VALIDATION.md)，使用/测试/调试见 [M2使用指南](docs/M2_USAGE.md)。M1集成默认100连接，不以功能测试推断吞吐量。

## 运行文件上传

Ubuntu终端一（使用专用新目录，存在则自行确认用途和权限）：

```bash
mkdir -m 700 /tmp/chunkharbor-m2-data
./build/chunkharbor-server --upload --root /tmp/chunkharbor-m2-data --port 8787
```

Ubuntu终端二：

```bash
cd /path/to/ChunkHarbor
source_file=$(mktemp /tmp/chunkharbor-demo-XXXXXX)
printf 'ChunkHarbor M2 upload\n' > "$source_file"
./build/chunkharbor-cli upload --file "$source_file" --path examples/demo.txt --port 8787
sha256sum "$source_file" /tmp/chunkharbor-m2-data/files/examples/demo.txt
ctest --test-dir build -R 'upload_|application_pause' -V
```

预期 `UPLOAD OK`，两项摘要一致。目标已存在会拒绝覆盖；测试重跑应换逻辑路径。断连的未完成会话保留并计入上限，不能重启续传；发布中断可能需要后续M3修复，详见使用指南。

## 运行 HELLO 握手

Ubuntu终端一：`./build/chunkharbor-server --handshake --port 8787`。

另一个Ubuntu终端：`./build/chunkharbor-cli hello --port 8787 --client-name chunkharbor-cli`。

预期输出 `HELLO OK: version=1 server=chunkharbor-server capabilities=handshake ...`。握手仅协商版本和实际能力，**不代表认证或支持文件上传**。`--handshake` 与 `--echo` 互斥，完整字段、错误码与状态规则见 [握手契约](docs/HANDSHAKE.md)。握手测试可运行 `ctest --test-dir build -R handshake -V`。

## 运行 Echo

Ubuntu 终端一：

```bash
./build/chunkharbor-server --echo --port 8787
```

看到 `LISTENING 127.0.0.1:8787` 后，在另一个 Ubuntu 终端执行：

```bash
cd /path/to/ChunkHarbor
./build/chunkharbor-cli echo --host 127.0.0.1 --port 8787 --message "hello ChunkHarbor"
```

期望：`ECHO OK: hello ChunkHarbor`。服务端 Ctrl+C 或 SIGTERM 正常关闭；支持 `--idle-timeout-ms 30000`、`--max-connections 512`。`--port 0` 由系统分配端口并打印，供自动测试使用。

Echo 是独立测试模式：PING(16) 回 PONG(17)，DATA_CHUNK(5) 原样回显以测试大二进制帧；这些 payload 暂作不透明测试字节，不执行 JSON/文件块业务。其他合法但未实现的消息、非法头或超限长度会关闭连接；没有 HELLO/上传/认证语义。配置 JSON 仍只是样例，程序目前通过上述参数配置。

服务端只绑定loopback；Windows可用 `ssh -N -L 18787:127.0.0.1:8787 miniob-ubuntu` 建立隧道，再使用Qt连接原型连接18787进行HELLO检测（服务端选handshake或upload模式）。普通文本客户端不能替代帧客户端，GUI原型不上传文件。

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

使用兼容 C++17 的编译器和 Ninja（加入 PATH），无需 Qt。Windows 默认关闭 Linux server/CLI。以下构建纯C++协议与client_core，不代表Windows原生上传界面；Qt连接原型构建与SSH连接方法见M2_USAGE.md：

```powershell
cmake -S . -B build-windows-protocol -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCHUNKHARBOR_BUILD_SERVER=OFF -DCHUNKHARBOR_BUILD_CLI=OFF -DCHUNKHARBOR_BUILD_GUI=OFF
cmake --build build-windows-protocol
ctest --test-dir build-windows-protocol --output-on-failure
```

本机MinGW13.2共享测试及Qt6.8.3匹配Kit连接原型均已运行；Windows Qt经SSH转发到Ubuntu的真实握手已通过。MSVC、GUI上传及分发打包未验证。

## 实现边界

已拆分 `chunkharbor_protocol`、最小 `chunkharbor_client_core`、`chunkharbor_linux_net`。网络采用单线程 LT；多线程 Reactor 为后续可选扩展。每连接输入上限 1 MiB+32，输出上限 4 MiB，高/低水位 1 MiB/512 KiB；积压暂停读，降水位恢复读；每次事件收发有预算，连接数与空闲期有上限。容量限额不包括内核 socket 内存及短暂编码/扩容副本。

未实现：SQLite、重启续传/崩溃自动修复、增量备份、下载恢复、GUI上传、TLS/认证。M2使用最小同步追加回执，而非SQLite事务或恢复协议。完整规格见 [指南](CODEX_DEVELOPMENT_GUIDE.md)，当前结果见 [进度](docs/PROGRESS.md)、[M2验收](docs/M2_VALIDATION.md)。
