# ChunkHarbor

ChunkHarbor 计划以 Linux Reactor 服务端和 Windows Qt Widgets 客户端实现单向增量备份，Linux CLI 用于开发和自动化。当前已进入 M1，实际完成了单线程 LT 模式的事件循环基础组件；服务端和 CLI 仍只有 `--help`，尚无网络监听或文件传输功能。Windows GUI 与跨平台协议库尚未实现。

## 构建和测试

在 Ubuntu 22.04 上安装 CMake、C++ 编译器和 Make，然后运行：

```bash
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 2
ctest --test-dir build --output-on-failure
./build/chunkharbor-server --help
./build/chunkharbor-cli --help
```

本轮新增的 `eventloop_test` 通过管道 FD 验证可读事件、跨线程投递和移除监听；也可单独运行：

```bash
ctest --test-dir build -R eventloop_test --output-on-failure
```

调试事件循环可用 `ctest --test-dir build -R eventloop_test -V` 查看测试过程，或用 `gdb ./build/chunkharbor-eventloop-test` 设置断点（例如 `break chunkharbor::EventLoop::run`、`break chunkharbor::EpollPoller::poll`），再执行 `run` 和 `bt`。目前没有 TCP 监听端口，因此还不能用客户端发起网络调试。

如果已安装 Ninja，可以将生成器改为 `-G Ninja`。`configs/server.example.json` 是后续阶段的配置示例，目前程序不会读取它。开发期服务端计划只监听 `127.0.0.1`。

新版指南中的 `CHUNKHARBOR_BUILD_*` 选项、Windows Qt 构建命令和独立协议 target 尚未落实到当前 CMake；现阶段请使用上方已验证过的 Linux 命令。Windows GUI 计划在 M2 完成基础上传后建立原型，并在 M5 完成实际业务集成。现有 Linux 专用源码不能作为 Windows GUI target 编译。

范围和验收标准见 [开发指南](CODEX_DEVELOPMENT_GUIDE.md)，实际进度见 [进度记录](docs/PROGRESS.md)。
