# R2.6 直接 TCP 与 SSH 隧道

## 直接 TCP

在 Ubuntu 执行 `ip -4 -br addr`，找虚拟网卡的 IPv4。用已有测试备份根启动服务，将下面的示例地址替换为该网卡的实际地址。

```bash
./build/chunkharbor-server --upload --root /existing/test-root --bind 192.168.80.129 --port 8787
```

Windows GUI 在「设置 → 连接方式」选择「直接 TCP（局域网 / 虚拟机）」，填写 Ubuntu IPv4 或能解析到该 IPv4 的主机名，填写服务端实际监听端口，再点击「检查连接」。检查会执行 ChunkHarbor HELLO。Windows 直接访问 Ubuntu 地址，无需本地 18787 转发端口。

VMware NAT 或 Host-only 下，Windows 宿主机必须能路由到虚拟网卡所在网段，Ubuntu 也须监听该地址。先看 Ubuntu 的 `LISTENING` 和 `ss -ltn`，再从 Windows 用 `Test-NetConnection -ComputerName <Ubuntu-IP> -Port 8787` 检查 TCP。连接拒绝时核对监听地址及端口，超时时检查网络路径及防火墙，TCP 可达但 HELLO 失败时核对端口对应的程序和版本。本项目没有 TLS 和认证，直连仅用于可信测试网络。

## SSH 隧道

Ubuntu 默认监听 127.0.0.1。SSH 模式可明确传入 `--bind 127.0.0.1`，也可沿用原有命令。在 Windows PowerShell 保持以下转发运行。

```powershell
ssh -N -o ExitOnForwardFailure=yes -L 18787:127.0.0.1:8787 miniob-ubuntu
```

GUI 选择「SSH 隧道（本地转发）」并填写本地端口 18787。GUI 只连接本地转发端点，SSH 进程由用户维护。服务端若只绑定虚拟网卡，SSH 命令的远端目标也可以改为该网卡地址，例如 `-L 18787:192.168.80.129:8787`。继续使用回环转发命令时，服务端须能在 Ubuntu 回环上接受连接。`0.0.0.0` 会监听所有 IPv4 接口，默认始终为回环。

## 实现与边界

`TcpServer` 用 `inet_pton` 校验 IPv4，再由原有非阻塞监听 socket 和 epoll 接管。先完成 bind/listen，再打开备份根。两端复用原有帧格式、SHA-256 和恢复发布。Qt 使用 IPv4 TCP 并显式绕过系统代理，健康检查和各业务页面接收同一配置。QSettings 分别保存两个配置，保留旧 SSH 端口并默认回到 SSH。配置改变停止原健康复查并使预览失效。上传、文件下载、目录恢复及正在确认的快照创建期间锁定连接设置。

SSH 检查点继续使用原有 `127.0.0.1:port` 标识。直连使用包含 `tcp:`、主机和端口的标识，匹配后仍执行原会话、源内容和版本基线校验。切换不清理、不重写旧状态。直连 DHCP 地址变化或用户改用同一服务器的另一个主机名时，也会产生不同标识。

Linux CLI 的既有使用方式见 [M4 使用指南](M4_USAGE.md)。本轮支持 Windows Qt GUI 直连，Ubuntu 服务端无需 Qt。

## 文件职责

| 范围 | 文件 |
| --- | --- |
| 服务端参数、校验与绑定 | `apps/server/main.cpp`、`include/chunkharbor/net/TcpServer.h`、`src/net/TcpServer.cpp` |
| GUI 配置、健康检查与页面传播 | `SettingsPage.h/.cpp`、`MainWindow.cpp`、`QtTcpTransport.cpp` |
| 任务配置、检查点与读取端点 | `TransferTypes.h`、`TransferController.cpp`、`BackupWorker.cpp`、`FileListController.cpp`、`DownloadController.cpp`、`SnapshotRestore.cpp`、`SnapshotPage.h/.cpp` |
| 回归测试及探针 | `gui_pages_test.cpp`、`gui_health_test.cpp`、`snapshot_page_test.cpp`、`gui_transfer_probe.cpp`、`gui_download_probe.cpp` |

## 验证记录

- Windows Qt 6.8.3 / MinGW 构建成功。原生平台完整 CTest **31/32**，100.04 秒。唯一失败为既有 `combo_popup_test` 的 `mouse opens actual popup`。配置持久化、旧 SSH 配置兼容、IPv4 主机名 HELLO、模式切换及业务回归通过。完整日志为 `build-windows-gui/r26-ctest-delivery.log`。
- Ubuntu 当前源码在隔离目录 `/tmp/chunkharbor-r26-D7Yvt6kV` 配置、构建通过，完整 CTest **57/57**，47.05 秒。另验证无参数与显式回环、虚拟机网卡 IPv4、显式全接口，以及非法地址、未分配地址、端口占用的错误。全接口仅在临时握手进程上测试。
- Windows Qt 测试探针使用 Ubuntu 私有 IPv4 直连，连接测试进程未建立 SSH 本地转发。验证了 GUI HELLO、回环绑定对私有地址不可达、首次上传、未变化 0 块、两次单块增量、文件列表、当前及历史单文件、目录快照与当前目录恢复。16 MiB 上传在持久 ACK 后中断，续传只发送余下 510/512 块，Ubuntu 文件 SHA-256 为 `341aacac661ccb210720bedaa9ead5d668fe5ea41a73532fc147c71e34040df1`。
- 同一套 Windows 测试探针用 SSH 转发和显式回环服务端重新运行闭环。两种模式的文件、目录逐项匹配内容及 SHA-256，旧目录来自固定快照，当前目录来自新快照。真实联调通过 Qt 自动输入，使用独立测试备份根，未对正式服务做部署操作。
- Windows 联调日志在 `build-windows-gui/r26/direct-delivery.log` 和 `build-windows-gui/r26/ssh-final.log`。Windows 的构建、CTest 及测试探针也保存在同一构建目录。手工原生设置页操作、其他 VMware 网段或防火墙配置、跨端点续传未验证。
