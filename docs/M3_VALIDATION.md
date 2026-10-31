# M3 验证与人工验收（2026-11-01）

M3 代码、自动化验证与用户执行的 Windows CLI 中断、Ubuntu 服务端重启、Windows CLI 续传组合验收均已完成。测试只使用新建的专用目录和随机源文件。服务端仅监听 `127.0.0.1`，Windows 经 SSH 隧道访问。

## 用户人工验收结果（2026-11-01）

- 用户在全新私有测试根目录启动 Ubuntu 服务端；Windows CLI 使用 SSH 本地转发上传32MiB随机文件，块大小65536字节，合计512块。中断前终端显示8行ACK。
- 同一服务端根目录重启后，Windows CLI `status` 返回 `MISSING_BLOCKS=503`；随后同一会话 `upload` 返回 `UPLOAD OK blocks_sent_this_run=503`。这说明本轮只补发服务端报告缺失的503块。
- Windows `Get-FileHash` 与 Ubuntu `sha256sum` 均为 `8db478eacc5c97f0d242e2145fe19af38a33a541ead56179380b93a20e80e3ff`。随后只读检查SQLite，状态为 `COMMITTED`，文件大小33554432、块大小65536、确认块数512。
- `512 - 503 = 9` 个块在服务端重启前已持久确认；客户端中断前只打印8行ACK，可能还有一块已提交但回执尚未打印。查询以服务端持久状态为准，续传结果与之相符。

## 已执行的自动验证

- Ubuntu 22.04 稳定隔离目录 `/home/ning/ChunkHarbor-m3-verify`：Debug 配置、构建与 CTest **30/30通过**；最新代码的 ASan/UBSan 和 Release 配置、构建与 CTest 也各 **30/30通过**。最新暂存清理改动后再次运行完整 Debug 回归，**30/30通过**。
- Ubuntu `resume_integration`：确认块后强制停止测试服务端，重启查询缺失位图、补块、重复请求、整文件摘要；还覆盖无效会话、损坏暂存块、SQLite 插入故障不发 ACK、无数据库记录目录隔离、数据库记录缺失暂存文件、过期清理和已提交目标文件丢失时拒绝启动。
- Windows MinGW 13.1 / Qt 6.8.3：`build-windows-m3` 配置/构建/CTest **9/9通过**；已有 GUI 原型构建/CTest **9/9通过**。`python tests/integration/windows_cli_ssh_test.py build-windows-m3/chunkharbor-cli.exe miniob-ubuntu /home/ning/ChunkHarbor-m3-verify/build/chunkharbor-server` 经 SSH 隧道实际上传并核对 Ubuntu SHA-256，再中断 CLI、通过状态文件和历史 session ID 查询、验证源文件变化会拒绝续传、恢复原源文件后只补缺失块，测试通过。
- 自动测试和上面的用户组合人工验收均已通过。尚未实测硬件掉电、物理磁盘满、TSan、性能基准或 Windows GUI 上传。

虚拟机缺少系统安装的 `libsqlite3-dev`，验证时在 `/tmp/chunkharbor-sqlite-dev/extracted` 使用匹配版本的开发头文件和运行库，并沿用已有私有 OpenSSL 头文件缓存；没有修改系统安装。普通 Ubuntu 环境可安装 `libssl-dev libsqlite3-dev` 后按 README 构建。

稳定验证目录的 SQLite 开发头已复制到 `/home/ning/ChunkHarbor-m3-verify/deps/sqlite/usr/include`。该目录实际配置命令（`build-asan` 另加 `-DCHUNKHARBOR_SANITIZERS=ON`，Release 使用 `-DCMAKE_BUILD_TYPE=Release`）：

```bash
cd /home/ning/ChunkHarbor-m3-verify
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DSQLite3_INCLUDE_DIR=/home/ning/ChunkHarbor-m3-verify/deps/sqlite/usr/include \
  -DSQLite3_LIBRARY=/usr/lib/x86_64-linux-gnu/libsqlite3.so.0 \
  -DOPENSSL_INCLUDE_DIR=/home/ning/ChunkHarbor/build/deps/openssl/usr/include \
  -DOPENSSL_CRYPTO_LIBRARY=/usr/lib/x86_64-linux-gnu/libcrypto.so.3 \
  -DCMAKE_CXX_FLAGS=-I/home/ning/ChunkHarbor/build/deps/openssl/usr/include/x86_64-linux-gnu
cmake --build build -j 2
ctest --test-dir build --output-on-failure
```

## 用户人工验收复现步骤：断开上传、重启服务端、继续上传

以下步骤可用于复现已通过的人工验收。使用 Windows PowerShell 和 Ubuntu 虚拟机三个终端；不要使用真实备份目录。每个终端在命令后保留输出。

1. Ubuntu 终端 A：进入本轮 M3 验证工程，创建**全新专用根目录**，启动服务端。`root` 变量留在同一终端，供第 5 步复用。

   ```bash
   cd /home/ning/ChunkHarbor-m3-verify
   root=$(mktemp -d /home/ning/chunkharbor-m3-accept-XXXXXXXX)
   printf 'TEST_ROOT=%s\n' "$root"
   ./build/chunkharbor-server --upload --root "$root" --port 8787
   ```

   预期看到 `LISTENING 127.0.0.1:8787`。记录 `TEST_ROOT` 的路径。不要把 `--root` 指向已有备份数据。

2. Windows PowerShell 终端 B：建立 SSH 本地转发，并保持窗口运行。

   ```powershell
   ssh -N -L 18787:127.0.0.1:8787 miniob-ubuntu
   ```

   正常情况下终端保持占用且不打印错误。若 18787 已被占用，换一个空闲本地端口，并在后续命令里同步替换。

3. Windows PowerShell 终端 C：在 Windows 仓库根目录配置 Qt Kit 和 CLI，生成专用随机源文件。此步骤只创建新目录与新文件。

   ```powershell
   cd C:\Users\Jianing\Desktop\C++Project\ChunkHarbor
   $env:PATH = 'D:\QT\Tools\mingw1310_64\bin;D:\QT\6.8.3\mingw_64\bin;' + $env:PATH
   cmake -S . -B build-windows-m3 -G Ninja -DCMAKE_MAKE_PROGRAM='D:/CLion 2023.3.6/bin/ninja/win/x64/ninja.exe' -DCMAKE_CXX_COMPILER='D:/QT/Tools/mingw1310_64/bin/g++.exe' -DCMAKE_PREFIX_PATH='D:/QT/6.8.3/mingw_64' -DCHUNKHARBOR_BUILD_SERVER=OFF -DCHUNKHARBOR_BUILD_CLI=ON -DCHUNKHARBOR_BUILD_GUI=OFF -DCHUNKHARBOR_BUILD_HASH=OFF
   cmake --build build-windows-m3 -j 2
   $work = Join-Path $env:TEMP ('chunkharbor-m3-accept-' + [guid]::NewGuid().ToString('N'))
   New-Item -ItemType Directory -Path $work | Out-Null
   $source = Join-Path $work 'source.bin'
   $state = Join-Path $work 'session.json'
   python -c "import os,sys; f=open(sys.argv[1],'xb'); f.write(os.urandom(32*1024*1024)); f.close()" $source
   ```

   预期配置、构建退出码为 0，`$source` 文件为 32 MiB。`$work`、`$source`、`$state` 留在终端 C 的变量中。

4. 终端 C 启动上传；看到至少几个 `ACK blocks_sent_this_run=...` 后按 **Ctrl+C** 中断。块大小 65536 字节，延时用于留出操作时间。

   ```powershell
   .\build-windows-m3\chunkharbor-cli.exe upload --file $source --path manual/source.bin --state $state --port 18787 --chunk-size 65536 --delay-ms 150
   ```

   预期先看到 `NEW session=<32字符ID>`，然后多行 `ACK`。中断后 `$state` 仍存在。源文件共有 512 块；若已经输出 `UPLOAD OK`，需重新创建专用根目录和源文件再做一次。

5. 终端 A 按 **Ctrl+C** 停止服务端，再在**同一终端**运行相同启动命令；不要新建根目录。终端 B 的隧道保持运行。

   ```bash
   ./build/chunkharbor-server --upload --root "$root" --port 8787
   ```

   预期再次看到 `LISTENING 127.0.0.1:8787`。若启动失败，请保留服务端 stderr，不要手工修改 `.sessions` 或 SQLite 文件。

6. 终端 C 先查询，再续传；`--state`、`--file`、`--path` 和块大小与第 4 步保持一致。

   ```powershell
   .\build-windows-m3\chunkharbor-cli.exe status --state $state --port 18787
   .\build-windows-m3\chunkharbor-cli.exe upload --file $source --path manual/source.bin --state $state --port 18787 --chunk-size 65536
   ```

   预期 `status` 打印 `RESUME session=<相同ID>` 和 `MISSING_BLOCKS=N`，其中 **0 < N < 512**。续传打印相同 session ID，最终 `UPLOAD OK blocks_sent_this_run=N sha256=...`；`N` 小于 512 证明没有从头发送所有块。成功后状态文件会移除。

   可将第 4 步的 `<相同ID>` 填入以下命令查询已完成的历史会话；预期输出 `COMMITTED session=... sha256=...`：

   ```powershell
   $sessionId = '把第4步显示的32字符ID填在这里'
   .\build-windows-m3\chunkharbor-cli.exe status --session-id $sessionId --port 18787
   ```

7. 比较两端 SHA-256。Windows 终端 C：

   ```powershell
   (Get-FileHash -Algorithm SHA256 $source).Hash.ToLowerInvariant()
   ```

   Ubuntu 终端 A 停止服务端后，沿用同一终端的 `$root` 变量；或另开终端并把第 1 步打印的路径赋给 `root`：

   ```bash
   sha256sum "$root/files/manual/source.bin"
   ```

   预期两串 64 位十六进制摘要完全相同。不要执行会覆盖该目标文件的第二次新上传。

## 失败时反馈

请提供：失败步骤号、运行的命令及退出码、Windows CLI 完整 stdout/stderr、服务端启动或重启时的 stdout/stderr、`status` 的 `MISSING_BLOCKS`、两端 SHA-256，以及 `TEST_ROOT` 下 `.sessions` 和 `.quarantine` 的文件名列表。状态文件包含本地路径；分享前可遮盖个人目录名和源文件内容。不要复制私钥或真实备份文件。
