# ChunkHarbor

C++17 单向增量备份项目。**M1–M4已实现并通过自动验收：自主Linux Reactor、SQLite持久会话、断点续传、固定块增量、只读差异预览及目录批任务。** Linux CLI和Windows Qt CLI支持`backup`/`preview`，Windows经SSH真实增量上传和服务端SIGKILL后续传已验证。GUI仍仅为连接原型；下载恢复、TLS/认证未实现。单文件上限64MiB，仅用于可信loopback/SSH开发。

M4使用、详细人工复现及自动恢复测试见[M4使用](docs/M4_USAGE.md)；真实构建/测试证据见[M4验证](docs/M4_VALIDATION.md)。旧`upload`命令保持首次上传/已有目标拒绝的语义；重复增量备份使用`backup`。版本对象与最新副本分别存储，尚无历史版本GC或总空间配额。

## 增量备份快速开始

启动下方 Ubuntu 服务端后，在另一终端对专用测试文件执行：

```bash
work=$(mktemp -d /tmp/chunkharbor-cli-XXXXXXXX)
printf 'first backup\n' > "$work/source.txt"
./build/chunkharbor-cli preview --file "$work/source.txt" --path demo/source.txt --state "$work/session.state"
./build/chunkharbor-cli backup --file "$work/source.txt" --path demo/source.txt --state "$work/session.state"
./build/chunkharbor-cli backup --file "$work/source.txt" --path demo/source.txt --state "$work/session.state"
```

第二次备份预期 `blocks_sent_this_run=0`。Windows CLI、SSH 隧道、目录备份与中断续传步骤见 [M4 使用指南](docs/M4_USAGE.md)。

## Ubuntu 构建与验证

在 Ubuntu 的仓库根目录构建，无需 Qt。依赖 CMake 3.20+、GCC、Make、Python 3、OpenSSL 与 SQLite 开发包（Ubuntu `libssl-dev libsqlite3-dev`）；GDB 为可选调试工具。

```bash
cd /path/to/ChunkHarbor
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 2
ctest --test-dir build --output-on-failure
```

M3自动验证包含SQLite事务、重启查询、损坏暂存块、数据库写入故障和Windows CLI经SSH的上传与续传。实际测试命令、结果及人工验收步骤见 [M3 验收](docs/M3_VALIDATION.md)；历史结果见 [M2 验收](docs/M2_VALIDATION.md)。M1集成默认100连接，不以功能测试推断吞吐量。

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

预期 `UPLOAD OK`，两项摘要一致。目标已存在会拒绝覆盖；测试重跑应换逻辑路径。断连的未完成会话可通过查询恢复；Windows CLI 的会话保存和重启续传步骤见 [M3 验收](docs/M3_VALIDATION.md)。未完成会话在30天后于服务端启动时过期并清理暂存文件。

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

使用兼容 C++17 的编译器和 Ninja（加入 PATH），无需 Qt。Windows 默认关闭 Linux server/CLI。Qt Windows CLI 的构建与使用见 [M3 验收](docs/M3_VALIDATION.md)。以下构建纯C++协议与client_core，不代表Windows原生上传界面；Qt连接原型构建与SSH连接方法见M2_USAGE.md：

```powershell
cmake -S . -B build-windows-protocol -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCHUNKHARBOR_BUILD_SERVER=OFF -DCHUNKHARBOR_BUILD_CLI=OFF -DCHUNKHARBOR_BUILD_GUI=OFF
cmake --build build-windows-protocol
ctest --test-dir build-windows-protocol --output-on-failure
```

本机MinGW13.2共享测试及Qt6.8.3匹配Kit连接原型均已运行；Windows Qt经SSH转发到Ubuntu的真实握手已通过。MSVC、GUI上传及分发打包未验证。

## 实现边界

已拆分 `chunkharbor_protocol`、最小 `chunkharbor_client_core`、`chunkharbor_linux_net`。网络采用单线程 LT；多线程 Reactor 为后续可选扩展。每连接输入上限 1 MiB+32，输出上限 4 MiB，高/低水位 1 MiB/512 KiB；积压暂停读，降水位恢复读；每次事件收发有预算，连接数与空闲期有上限。容量限额不包括内核 socket 内存及短暂编码/扩容副本。

未实现：下载恢复、GUI上传、TLS/认证。M4使用schema v2、不可变版本、固定基线和条件发布；文件系统与SQLite之间的发布窗口通过启动核验协调，不能视为跨存储原子事务。新库不能交给旧二进制；真实旧备份根升级前应先备份并审核。旧M2无数据库会话仍隔离。完整规格见[指南](CODEX_DEVELOPMENT_GUIDE.md)，当前结果见[进度](docs/PROGRESS.md)、[M4验证](docs/M4_VALIDATION.md)。
