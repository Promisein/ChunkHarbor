# Windows GUI 构建与使用

当前GUI为Qt6 Widgets真实备份客户端，沿用M4协议。只连接本机或SSH隧道；无需在Windows编译Linux服务端。

## CLion

选择现有Windows-GUI配置及匹配的MinGW/Qt Kit。CMake选项：

```text
-DCHUNKHARBOR_BUILD_SERVER=OFF -DCHUNKHARBOR_BUILD_CLI=OFF -DCHUNKHARBOR_BUILD_GUI=ON -DCHUNKHARBOR_BUILD_HASH=OFF -DCHUNKHARBOR_BUILD_TESTS=ON -DCHUNKHARBOR_DEPLOY_GUI=ON -DCMAKE_PREFIX_PATH=D:/QT/6.8.3/mingw_64
```

重新加载CMake，选择chunkharbor-gui | Windows-GUI，构建并运行。当前实现不需要额外Qt模块，也不需要Windows OpenSSL开发包；Qt和MinGW必须来自同一Kit。部署开关自动复制DLL/平台插件，不等同于正式安装包。

本仓库已验证的命令（项目根目录PowerShell）：

```powershell
$env:PATH='D:\QT\Tools\mingw1310_64\bin;D:\QT\6.8.3\mingw_64\bin;'+$env:PATH
cmake --build build-windows-gui -j 2
ctest --test-dir build-windows-gui --output-on-failure
.\build-windows-gui\chunkharbor-gui.exe
```

CLion若使用cmake-build-windows-gui目录，替换上面的构建目录名即可。首次配置可使用根目录AGENTS.md的Windows命令，并按实际SDK位置调整。

## 连接Ubuntu

Ubuntu原工程`/home/ning/ChunkHarbor`现已同步最新M6并在原`build`完成构建和50/50回归，日常使用原工程，不需要新建源码工程目录。进入`cd /home/ning/ChunkHarbor`后使用`./build/chunkharbor-server`和`./build/chunkharbor-cli`。同步方式及未提交状态见UBUNTU_SYNC.md；此前临时验证工程路径只代表历史测试环境，不应继续作为日常入口。

同日后续清理已将旧验证目录的SQLite开发包移至主项目`/home/ning/ChunkHarbor/build/deps/sqlite`，旧`ChunkHarbor-m3-verify`已删除。未来Ubuntu配置若需要手动指定SQLite头，使用`-DSQLite3_INCLUDE_DIR=/home/ning/ChunkHarbor/build/deps/sqlite/usr/include`；历史验收文档记录的旧路径仅代表当时环境。

Ubuntu项目根目录，使用新建专用目录（保留该终端及输出的根路径）：

```bash
backup_root=$(mktemp -d /tmp/chunkharbor-gui-manual-XXXXXXXX)
printf '测试备份根目录：%s\n' "$backup_root"
./build/chunkharbor-server --upload --root "$backup_root" --port 8787
```

真实备份必须使用`--upload`，`--handshake`只支持连接检测。若已启动服务端，直接使用已有端口和原备份根，不另起同端口服务。

Windows PowerShell另开终端，保持隧道运行：

```powershell
ssh -N -o ExitOnForwardFailure=yes -L 18787:127.0.0.1:8787 miniob-ubuntu
```

没有该SSH别名时，替换为`Ubuntu用户名@Ubuntu地址`。GUI设置端口18787，点击检查连接。修改端口会失效旧在线状态，任务运行期间设置锁定。

## 手工体验真实备份

### 三种备份模式

| 模式 | 用途 | 已有文件与会话 |
| --- | --- | --- |
| 首次完整上传 | 将文件首次备份到新目标 | 保护其他任务已有目标，不覆盖；同任务完成记录与源配置匹配时可重新核验为未变化 |
| 断点续传 | 继续上次未完成的任务 | 查询服务端会话，仅补缺失块；源、端点、逻辑路径和分块参数必须与原会话匹配 |
| 增量备份 | 日常重复备份 | 新文件完整上传，已有文件比较同位置固定块，仅传变化块；无变化为0数据块 |

三种模式都先扫描并预览，再开始备份。中断后先用原配置续传；未完成源内容变化时不能盲续，恢复原内容/配置或使用新远端路径。GUI中“增量备份”用于通常的后续更新，“断点续传”用于恢复未完成会话，不是另一套加密或传输协议。

在另一Windows PowerShell创建专用文件，不使用真实文档：

```powershell
$guiTestDir=Join-Path $env:TEMP ('ChunkHarbor-Gui-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $guiTestDir | Out-Null
$guiFile=Join-Path $guiTestDir 'source.bin'
$guiBytes=New-Object byte[] (4*1024*1024)
$guiRng=[Security.Cryptography.RandomNumberGenerator]::Create()
$guiRng.GetBytes($guiBytes)
$guiRng.Dispose()
[IO.File]::WriteAllBytes($guiFile,$guiBytes)
$guiTestDir
Get-FileHash -Algorithm SHA256 -LiteralPath $guiFile
```

1. GUI备份页选择输出的目录，远端路径填`manual`，模式选增量备份。点击扫描并预览，出现真实新增文件后点击开始备份。
2. 查看任务阶段、持久确认字节；文件/版本页出现真实提交记录，日志页显示实际事件。
3. 再次扫描并备份，预期未变化、本轮发送0块。0块仍需服务端校验并确认提交。
4. 在同一PowerShell修改单个字节，再扫描：

```powershell
$guiBytes=[IO.File]::ReadAllBytes($guiFile)
$guiBytes[300000]=$guiBytes[300000] -bxor 1
[IO.File]::WriteAllBytes($guiFile,$guiBytes)
Get-FileHash -Algorithm SHA256 -LiteralPath $guiFile
```

默认256KiB块下该文件预计只有1个块变化。Ubuntu另开终端，使用第一步打印的实际根目录校验：`sha256sum /实际测试备份根/files/manual/source.bin`，应与Windows摘要相同。

5. 续传体验可将专用文件增大至32MiB（单文件不超过64MiB），设置64KiB块，上传期间暂停/取消或关闭窗口。重开后保持原目录、前缀、端口与块大小，先扫描，再用断点续传开始，或在同一次运行中点击重试。若上传已完成，就不能将它当作中断测试。

无需把测试结果作为继续开发的批准；以上供实际交互体验。自动测试、恢复与SHA证据见M5_VALIDATION.md。

## 边界

- 首次完整上传用于目标尚不存在；已有目标会拒绝。日常重复备份用增量模式。
- 暂停在安全请求边界生效；取消/关窗保留服务端已确认进度。预览后源或远端基线变化会拒绝执行，需重新扫描。
- 历史/文件页仅展示本设备已核验提交的记录，最多保留500条本地历史；不是全服务器目录。下载恢复按钮禁用，留给M6。
- 默认浅色、单通道顺序上传、SHA-256；不提供虚假的并行或其他校验模式。设置中的令牌占位禁用，认证留给M7。
- 会话、历史与日志在用户AppLocalDataLocation下；非秘密偏好使用QSettings。状态绑定端点、源摘要和配置，不应手工编辑会话文件。默认日志滚动保留最近500条，可导出当前筛选结果。
