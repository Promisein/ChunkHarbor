# M2 文件上传：使用、测试和调试

M2已实现：Linux CLI→自主Reactor服务端，握手→声明→计划→分块SHA-256→持久块回执→整文件SHA-256→同文件系统不覆盖发布。Windows另有Qt最小连接/握手原型，不提供上传界面。

## 编译

Ubuntu依赖C++17编译器、CMake、Make、Python3、OpenSSL开发包；无需Qt或SQLite。正式目录：

```bash
ssh miniob-ubuntu
cd /home/ning/ChunkHarbor
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 2
ctest --test-dir build --output-on-failure
```

当前VM没有免密sudo，OpenSSL使用此前已下载到忽略的build/deps里的匹配开发头和系统libcrypto；当前构建目录已有缓存。新机器可安装libssl-dev，或按 [SHA256.md](SHA256.md) 的无root配置提供头/库路径，不将依赖二进制加入Git。

## 启动服务

在第一个Ubuntu终端创建专用新目录（若目录已存在，应自行确认其用途与权限，不覆盖原数据），并前台运行：

```bash
mkdir -m 700 /home/ning/chunkharbor-m2-data
cd /home/ning/ChunkHarbor
./build/chunkharbor-server --upload --root /home/ning/chunkharbor-m2-data --port 8787
```

预期打印 `LISTENING 127.0.0.1:8787`。根目录必须是现存的绝对路径、属于服务进程有效UID、group/other权限全部为0，路径各级无符号链接；需要Linux5.6+、支持O_TMPFILE的文件系统和可用/proc/self/fd。根目录有进程排他锁，一个根只允许运行一个服务器。默认32连接、最大64，2个磁盘工作线程、64个outstanding任务；空闲超时30秒，可用 `--idle-timeout-ms` 调整。Ctrl+C受控停止；退出会等待已接受的有限磁盘任务完成。

Echo及独立握手模式仍可运行，但不提供上传：`--echo`、`--handshake`、`--upload`三选一。服务端始终只监听127.0.0.1；没有TLS/认证，不应暴露到不可信网络。

## 上传并校验

第二个Ubuntu终端：

```bash
cd /home/ning/ChunkHarbor
source_file=$(mktemp /tmp/chunkharbor-demo-XXXXXX)
printf 'ChunkHarbor M2 upload\n' > "$source_file"
./build/chunkharbor-cli upload --file "$source_file" --path examples/demo.txt --port 8787
sha256sum "$source_file" /home/ning/chunkharbor-m2-data/files/examples/demo.txt
cmp "$source_file" /home/ning/chunkharbor-m2-data/files/examples/demo.txt
```

进度中的 `sent` 在完整帧发送后增加，`confirmed` 只在收到并验证对应持久ACK后增加。最终预期 `UPLOAD OK path=examples/demo.txt size=... sha256=...`；两条sha256sum一致，cmp退出0。最终文件位于根目录下的 `files/<逻辑路径>`，原始源文件不被修改。

路径必须为有效UTF-8相对逻辑路径、统一 `/` 分隔，不可传绝对路径、盘符、`..`或反斜杠。`--file`与`--path`分开，后者不是服务器绝对路径。默认块256KiB，可选 `--chunk-size 4096` 等1..1048516字节；单文件最多64MiB，最多65536块。空文件无需DATA_CHUNK也可提交，末块允许较短。

目标已存在时返回CONFLICT，保持原文件不变；不会静默覆盖。重复演示请使用新的逻辑路径或新的测试根，不能把重跑同一路径视为增量备份。CLI使用同一打开的普通文件FD，拒绝符号链接源，检查size/dev/ino/mtime/ctime；源在上传中变化会失败，服务器只有整文件摘要一致才发布。CLI只允许loopback；跨主机开发可经SSH隧道，但Windows GUI上传仍未实现。

## ACK、提交和保留状态

服务端 `begin` 将匿名暂存文件链接到私有 `.sessions/<随机ID>/data`，持久化会话meta和目录项。每块顺序为完整pwrite→fdatasync(data)→追加ACK回执→fdatasync(receipts.log)→投递回loop→发送CHUNK_ACK。重复相同块可返回同义ACK，不同内容拒绝覆盖。磁盘操作、哈希和fsync不在EventLoop执行。

这份同步追加回执是M2的最小持久元数据，不是SQLite事务、恢复查询或M3断点续传。每连接只上传一个文件，一次只有一个磁盘请求；应用读暂停独立于输出背压，完成后延后重放缓冲，避免在executor容量尚未释放时误报BUSY。

提交要求所有块已确认，重新读取整文件SHA-256并匹配，再同步data、写COMMITTING记录、renameat2(NOREPLACE)到安全目标目录、同步目标和来源目录、写COMMITTED记录，最后返回COMMIT_RESULT。已有文件/链接不能被覆盖。客户端检查commit的request_id、类型、session、path、size和sha256，不能仅凭收到一个响应就报告成功。

正常成功后只清理服务自己创建的session元数据，保留正式文件；连接断开或部分失败后，已确认的暂存数据和回执不会自动删除。最多保留32个未完成会话，按声明数据大小预留合计最多512MiB；这不包含有界回执/文件系统开销，也不是正式文件总容量配额。达到限额拒绝新上传，不无限增长内存或打开FD。重启会检查保留记录并继续计入配额及同路径冲突，但**不会恢复为可继续上传的会话**。

若发布/目录同步/清理在中途失败，可能出现已校验文件已改名但客户端未收到成功、COMMITTING记录或部分清理状态。启动对此拒绝继续服务，等待后续M3协调；不会猜测成功或删除已有数据。保留整个根目录供离线检查，演示可另建新的空测试根。不要在文档外手工修改/递归删除真实备份数据。本轮不声称断电后自动恢复或跨文件系统/元数据的原子事务。

## 自动化测试

```bash
ctest --test-dir build --output-on-failure
ctest --test-dir build -R 'upload_|application_pause' -V
```

重点测试：upload_control（严格提交/错误消息）、upload_storage（实际哈希、部分写/EINTR/零写、数据/回执同步失败、ENOSPC、FIFO拒绝、锁/配额/保留扫描、不覆盖及发布同步窗口）、upload_handler（容量1下粘包重复提交）、upload_integration（真实CLI、空/二进制/中文/短末块、并发、异常输入、断连、重启后新任务）、cli_upload_peer_test（伪造ACK/commit、截断响应、源文件变更）。原有M1网络与握手测试全部保留。

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCHUNKHARBOR_SANITIZERS=ON
cmake --build build-asan -j 2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-asan --output-on-failure
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j 2
ctest --test-dir build-release --output-on-failure
```

测试数据仅使用独占临时目录。故障通过链接包装注入，不等于真实磁盘满、断电或各种文件系统的覆盖；未运行TSan、真实掉电和kill -9恢复。Windows验证了共享库和下述连接原型，尚无Windows原生文件上传。

## Windows Qt连接原型

本机Qt6.8.3与匹配MinGW Kit已实际构建、运行8项CTest（包含7种GUI网络场景）；不是只写CMake。PowerShell中：

```powershell
cd C:\Users\Jianing\Desktop\C++Project\ChunkHarbor
$env:PATH="D:/QT/6.8.3/mingw_64/bin;D:/QT/Tools/mingw1310_64/bin;" + $env:PATH
cmake -S . -B build-windows-gui -G Ninja `
  '-DCMAKE_MAKE_PROGRAM=D:/CLion 2023.3.6/bin/ninja/win/x64/ninja.exe' `
  -DCMAKE_CXX_COMPILER=D:/QT/Tools/mingw1310_64/bin/g++.exe `
  -DCMAKE_PREFIX_PATH=D:/QT/6.8.3/mingw_64 -DCMAKE_BUILD_TYPE=Debug `
  -DCHUNKHARBOR_BUILD_GUI=ON -DCHUNKHARBOR_BUILD_SERVER=OFF `
  -DCHUNKHARBOR_BUILD_CLI=OFF -DCHUNKHARBOR_BUILD_HASH=OFF
cmake --build build-windows-gui -j 2
ctest --test-dir build-windows-gui --output-on-failure
```

Ubuntu服务器已监听8787后，在Windows另一个终端保持SSH转发运行：

```powershell
ssh -N -L 18787:127.0.0.1:8787 miniob-ubuntu
```

再在已设置Qt PATH的PowerShell运行 `./build-windows-gui/chunkharbor-gui.exe`，地址127.0.0.1、端口18787，点击Connect，预期显示Protocol ready。可连--handshake或--upload模式，只协商handshake能力，不扫描文件、不上传。--probe-port用于自动握手检测，--screenshot用于截图后退出；源码与后续边界见GUI_DESIGN.md。原型不是可分发安装包，Qt部署/打包留后续。

## 自己调试

```bash
gdb --args ./build/chunkharbor-server --upload --root /home/ning/chunkharbor-m2-data --idle-timeout-ms 300000
```

可设断点 `chunkharbor::UploadStorage::begin`、`chunkharbor::UploadStorage::write`、`chunkharbor::UploadStorage::commit`、`chunkharbor::TcpConnection::pauseReading`。用 `info threads` / `thread N` 查看工作线程，`bt` 看栈；另一终端运行CLI。工作函数在磁盘线程，发送响应在loop。只用专用测试根进行断点与故障实验。单测也可 `gdb --args ./build/chunkharbor-upload-storage-test`。

## C++要点

FD由UniqueFd管理，session由shared_ptr保留，连接完成回调使用weak_ptr，避免断连后解引用。每个session的mutex保护写入/提交和位图，全局mutex保护配额/同路径占用；多个会话可由不同worker处理。逐块缓存和64KiB整文件校验缓冲避免整文件驻留内存。总数据校验/写入为O(file_size)，会话位图/摘要为O(block_count)，同时受文件、块数、会话、连接和队列限额约束。
