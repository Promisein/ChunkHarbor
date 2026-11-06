# R2.6 补充：Windows SSH 隧道自动管理

日期：2026-11-05。开发基线 `aa22a6f`。本轮仅修改 Windows Qt GUI 连接管理、设置、测试和文档；不改 Linux Reactor、协议、SQLite v4、上传算法、哈希及恢复发布机制。未自动提交或推送。

## 使用流程

打开 GUI，恢复上次连接方式。直接 TCP 自动执行原 HELLO；SSH 模式自动启动 `ssh.exe`、验证本地监听归属、检查 HELLO。失败时界面保持可操作，可修改参数或点击“检查连接”；“断开连接”停止隧道及后续自动重试。

SSH 配置：主机/别名默认 `miniob-ubuntu`，用户留空使用现有 OpenSSH 配置，SSH 端口默认 22，本地转发端口沿用旧 `connection/sshPort`/`connection/port`（默认 18787），目标默认 Ubuntu `127.0.0.1:8787`。用户、主机、目标和端口分别保存在 QSettings；TCP 配置仍独立保存。不提供密码、令牌或私钥内容输入框。主机/目标使用有界 ASCII 别名、IPv4 或 DNS 名称；非法参数在启动前拒绝。

Ubuntu 必须已有运行中的备份服务。GUI 不远程启动服务、不修改备份库。旧手工转发占用端口时会明确报冲突；应用拒绝复用未知监听器，也不会结束外部 SSH 进程。可以关闭旧手工隧道，或为本程序选择空闲端口。

## 组件与线程归属

- `apps/gui/SshTunnelManager.{h,cpp}`：配置、状态机、QProcess、超时、重连、错误分类和 Windows 进程/监听归属。
- `apps/gui/MainWindow.{h,cpp}`：默认自动连接、顶栏与设置状态、业务可用性、模式切换和退出协调。
- `apps/gui/pages/SettingsPage.{h,cpp}`：SSH 参数、持久化、检查/断开操作；未验证的 SSH 端点不会交给备份和浏览任务。
- `apps/gui/main.cpp`：显式健康探针使用手动启动模式，避免重复 HELLO。
- `CMakeLists.txt`：加入组件、Windows IP Helper 系统库、新测试和探针。
- 新测试 `tests/unit/managed_ssh_test.cpp`，进程替身 `tests/integration/ssh_process_fixture.cpp`，真实探针 `managed_ssh_probe.cpp` 和 `windows_managed_ssh_test.py`。
- 既有 GUI、下载、历史、快照探针改为明确连接其测试拥有的 TCP 端点；严格协议故障夹具使用 `StartupMode::Manual`。不删除或降低原断言。产品默认 `Automatic`，真实自动 SSH 用例也使用默认路径。

QProcess、管理定时器和健康 QTcpSocket 均属于 GUI 线程，通过事件循环异步运行；没有 `waitForStarted`、`waitForFinished` 或阻塞网络调用。既有业务 worker 仍创建并使用自己的 socket、源文件句柄和安全输出组件，不复制传输实现。

## 启动与三个不同层次的就绪

1. 先检查参数、可执行文件和端口占用，再用 `QProcess::start(program, QStringList)` 直接启动，不拼接 shell 命令，不启动 PowerShell/CMD。
2. Windows 使用 `CREATE_NO_WINDOW` 与隐藏窗口标志。`started` 只代表进程建立；将本进程启动的子进程加入专有 Job Object，禁止 ControlMaster/ControlPersist 和认证后后台分叉，保留明确的生命周期归属。
3. 每 100ms 有界查询 IPv4 TCP listener 表，要求 `127.0.0.1:本地端口` 的 PID 与 QProcess 子进程一致。不能取得归属或发现竞争监听器时拒绝连接，终止的只有自己创建的进程。端口预检查和 bind 之间的竞争不靠“检查成功”放行。
4. 归属核验成功后进入 `TunnelReady`，再复用 QtTcpTransport 的 HELLO。只有真实 HELLO 成功才能进入 `Connected`，并允许新的业务请求。服务器未启动时可以明确显示“SSH 隧道已建立，但备份服务不可用”。

状态包含 Disabled、Starting、TunnelReady、CheckingServer、Connected、Failed、Reconnecting、Stopping。失去隧道时先退出 Connected，再通知观察者，避免旧成功状态授权新请求。健康探测继续按原有 15 秒周期执行；周期检查期间保留已验证隧道的在途业务，失败后禁止新的端点使用。

Windows PID 表查询使用 `GetExtendedTcpTable`，最多 1 MiB、最多三次缓冲重试，失败不降级为仅探测端口。[Microsoft API 文档](https://learn.microsoft.com/en-us/windows/win32/api/iphlpapi/nf-iphlpapi-getextendedtcptable)说明表类型与进程归属字段。Job Object 仅关联本组件创建的 SSH 及其子进程，句柄关闭会清理所属树，不处理外部 ssh-agent 或其他用户会话；基础语义见 [Job Objects](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects)。

## 参数、错误和重连

主要参数：`-N -T`、`BatchMode=yes`、`ExitOnForwardFailure=yes`、`ConnectTimeout=5`、`ConnectionAttempts=1`、`ServerAliveInterval=30`、`ServerAliveCountMax=3`、`StrictHostKeyChecking=yes`、`NumberOfPasswordPrompts=0`，以及 `-p`、可选 `-l`、显式回环 `-L` 和主机参数。禁止复用 SSH 控制连接、持久后台进程和 LocalCommand；stdin/stdout 指向空设备。

stderr 分块处理，滚动片段有界，单进程累计超过 64 KiB 停止；只把分类后的短错误写入 UI/日志，不展示原始 SSH 调试输出或私钥路径。认证、主机密钥、配置和端口冲突属于须用户处理的错误，不自动反复尝试。缺少 ssh.exe、无法执行、主机不可达和启动超时均有明确提示。

暂态网络失败或意外退出最多重试三次，默认间隔 1、2、4 秒。单次总启动 deadline 12 秒，SSH 自身连接 timeout 5 秒。连接稳定 60 秒后才重置重试预算，避免短暂上线反复清零造成无限高速重连。用户主动重试可以重新开始一轮；主动断开、切换模式和关窗取消自动重试。

同一配置重复启动不创建第二个进程；新配置等旧进程退出后再启动。旧进程停止阶段仅限量排空 stderr，不能把迟到的认证错误或 FailedToStart 覆盖为新配置失败。信号回调检查具体 QProcess 对象，定时启动再次核对当前意图。

隧道失效会取消在途备份/下载/恢复并使预览失效，保留已持久检查点与安全输出清理语义。重连成功仅恢复通信；不自动重新执行任务、不清除 checkpoint、不把旧预览重新标为可执行。

## 退出和所有权

关闭窗口先进入 closing 状态，停止延迟连接与重试，按原流程取消并等待文件任务、暂存清理和快照创建确认收敛，再请求 SSH 退出。`terminate()` 后由定时器在 1 秒后对本子进程 `kill()`；收到 finished 才接受最终关窗。没有通过 GUI 主线程阻塞等待子进程。强制销毁窗口还有 QProcess/Job 句柄兜底，但正常路径走异步收尾。

“立即关窗”“启动前主动断开”也会撤销构造时的延迟启动回调，不能在窗口关闭后建立孤儿隧道。若清理失败或快照提交状态不确定，沿用原有保持界面、报告暂存路径的行为。外部 SSH 不纳入 Job，不枚举后按名称批量杀进程。

Qt 的 `started`、`errorOccurred`、`finished` 提供进程事件，但 started 不证明网络服务就绪；参见 [QProcess](https://doc.qt.io/qt-6.8/qprocess.html)。这里用 PID 归属和 HELLO 补齐进程、隧道、业务三个层次。

## 一次性 SSH 配置

本机已有 `miniob-ubuntu` 别名及密钥，真实测试已复用它们。正常日常使用无需打开终端。

新电脑需要先安装 Windows OpenSSH Client，把公钥配置到 Ubuntu 账号，并核对服务器指纹后完成一次登录：

```powershell
ssh miniob-ubuntu
# 登录确认后退出；此命令用于一次性认证/信任配置，不是日常转发。
```

有口令的密钥可一次性配置 Windows ssh-agent 服务及 ssh-add，使非交互登录可用。应用不代存口令、不自动接受未知或变化的主机密钥。可用 `ssh -o BatchMode=yes miniob-ubuntu true` 检查配置是否满足非交互登录。参考 [Windows OpenSSH 密钥管理](https://learn.microsoft.com/en-us/windows-server/administration/openssh/openssh_keymanagement)。SSH 端口非 22 的别名需在 GUI 同步填写相应端口。

## 实际验证

测试环境：Windows Qt 6.8.3 / MinGW 13.1、Windows OpenSSH 9.5p2；Ubuntu 22.04 / GCC 11.4。全部使用专用临时状态、模拟端口或 `/tmp/chunkharbor-auto-ssh-data-*`，不打开真实备份库，不更改用户 SSH 配置。

Windows 全目标构建成功，最终完整 CTest **34/34**，250.68 秒，日志 `build-windows-gui/ssh-delivery-build.log`、`ssh-delivery-ctest.log`。其中 managed_ssh_test 15.58 秒、原生 ComboBox 40.43 秒；补入正式样式资源后，受影响的 managed_ssh_test 再跑 **1/1**（15.65 秒，`ssh-styled-test.log`）。测试耗时只作运行记录，不作为性能结论。

过程测试先证明旧设置缺少自动 SSH；lateauth 配置切换、立即关窗等竞态先复现失败，再修复。首轮完整测试中的旧下载/历史/快照单连接夹具因新增启动 HELLO 发生竞争，已用显式 Manual 启动测试模式隔离；原故障断言保持。原生 ComboBox 的对齐断言曾失败，记录到切换 SSH 显示新字段后控件横移 14 像素、测试在布局稳定前立刻点击；撤销无效限宽改动，仅在模拟输入前等待布局稳定，原鼠标、键盘、透明圆角、对齐和焦点断言全部保留，最终普通/最大化均通过。

新增用例覆盖进程启动不等于连接、监听不等于 HELLO、缺失 executable、认证/主机密钥错误、启动 deadline、端口预占和预检查之后的竞争监听、有限重试、主动停止重试、重复检查、快速模式切换、旧错误回调、窗口启动立即关闭/断开、普通/最大化状态几何、关窗后拥有的子进程确实退出。隔离替身为 console 类型可执行文件，启动时断言 `GetConsoleWindow()==nullptr`，验证没有额外控制台。

Windows 原生窗口布局另使用正式 Light.qss 资源和独立替身运行，截图 `build-windows-gui/ssh-settings.png`。这是自动化运行与截图检查，不是人工逐项验收，也不将替身截图当作 Ubuntu 连接证据。

Ubuntu 本轮未改服务端源码；实际重新运行既有完整 CTest **57/57**，44.80 秒，日志 `/tmp/chunkharbor-managed-ssh-CPp9vhud/ubuntu-ctest.log`。本轮没有构建 Linux Qt GUI。

真实测试命令：

```powershell
python tests/integration/windows_managed_ssh_test.py build-windows-gui/chunkharbor-managed-ssh-probe.exe miniob-ubuntu /home/ning/ChunkHarbor/build/chunkharbor-server
```

真实 Windows OpenSSH 自动管理测试 **4 组通过**，日志 `build-windows-gui/ssh-real-final.log`，最终专用服务根 `/tmp/chunkharbor-auto-ssh-data-RtwQOz9a`：

| 场景 | 实际结果 |
| --- | --- |
| backup | 应用自行启动 SSH、PID 归属核验、HELLO、3 文件 auto 备份、列表、最大文件指定历史版本下载，校验成功；只启动 1 个隧道；关窗后进程已退出 |
| reconnect | 只强杀本探针自己的 SSH 子进程，受控重连后再次 HELLO，备份预览/确认与历史下载通过；总启动数 2；关窗后进程已退出 |
| offline | SSH 主机认证和本地监听正常，Ubuntu 指定端口无备份服务；明确显示隧道建立而备份服务不可用，没有误报在线；启动数 1，关窗后进程已退出 |
| interrupt | 16 MiB 文件已有 ACK、上传尚未完成时强杀本程序 SSH；任务收尾、保留 1 个未完成检查点；第二个 SSH 重新 HELLO 后等待观察，任务仍未自动重跑且旧预览不可执行；关窗后 SSH 已退出 |

四组运行中 10ms GUI 心跳分别为 358、431、129、402；这里只证明该次事件循环持续运行。Python 测试只用普通 SSH 命令搭建 Ubuntu 专用测试服务，没有启动外部 `ssh -L`；所有本地转发进程由 GUI 的 QProcess 创建。第一轮强杀测试暴露了清理期间短暂发布过期 Connected 的问题，修复后才取得以上最终结果。活动上传故障探针另需等待替代进程真正启动并重新 HELLO，不能把 kill 后尚未收到 finished 的旧进程状态当作重连成功；该测试时序也已修正。探针调用生产 MainWindow、管理器和业务控制器，属于自动化端到端验证，不是人工逐项操作 GUI 对话框。

本地内容与服务端 sha256sum 一致，探针还对下载后的 1 MiB 文件独立计算 SHA-256：

| 文件 | SHA-256 |
| --- | --- |
| empty（0 字节） | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| multi.bin（1,048,576 字节） | `fbbab289f7f94b25736c58be46a994c441fd02552cc6022352e3d86d2fab7c83` |
| 中文.txt（UTF-8 文本） | `e77d8d73dac5ef35b4ffa1433dc13a8c5b59832b85fbecc067984818492d8a88` |

## 保留限制

- 自动 SSH 目前针对 Windows OpenSSH；不提供 SSH 交互终端、密码弹窗、密钥生成或远程启动服务。
- GUI 自动启动时若配置尚未完成会显示错误，仍可修改设置。重试耗尽后需用户检查连接，不能无限后台启动进程。
- 主机密钥不匹配和认证错误使用隔离替身验证，未改真实 known_hosts 或真实密钥来制造故障。VM 真正关机/重启、真实网络拔线、硬件掉电、MSVC、复杂 ProxyCommand、企业 Job 策略和长期运行未验证。
- 正常退出已覆盖；操作系统强杀父进程在 Job 分配前的极小窗口没有做独立故障注入，不宣称绝对不存在任何异常系统状态下的孤儿进程。
- 自动化原生窗口、几何和鼠标检查与人工逐项验收分别记录，不能互相代替。
