# R2.1 Windows GUI 当前单文件下载

范围仅为文件页的当前单文件下载；复用既有 download_v1，协议、schema、上传模式和健康检查不变。仍限制单文件64MiB、单任务、可信127.0.0.1/SSH；无下载续传、历史版本或目录恢复。

## 操作与协议

1. 刷新“文件”，选择一项，点击“下载”，通过系统保存框指定尚不存在的本地目标。取消保存框不会启动任务。
2. 保存框弹出前复制该行的逻辑路径、版本ID、大小、SHA-256和端点；筛选使用代理模型映射回原始行，不从显示文本重新拼版本。
3. 后台创建安全输出并连接；HELLO协商download_v1，BEGIN_DOWNLOAD(13)携带精确版本ID，DOWNLOAD_READY(32)必须与选择项的路径/版本/大小/摘要全部一致。VERSION_CONFLICT提示刷新列表，不回退到最新版本。
4. GET_DOWNLOAD_CHUNK(33)逐块拉取，DOWNLOAD_CHUNK(14)检查request_id、类型、版本、偏移、长度和块SHA-256。每块写入并累计整文件哈希后才请求下一块，空文件直接检查空内容摘要。
5. 整文件摘要匹配后FlushFileBuffers，再跨越发布提交点，执行系统级防覆盖发布。成功后显示保存位置；任何提交前失败/取消清理临时文件。

## 线程、所有权与边界

- DownloadController属于GUI线程，仅保存任务状态和发信号；独占一个std::thread，不在MainWindow塞业务逻辑。
- QTcpSocket在下载线程栈上创建、使用和销毁，阻塞等待仅在该线程；每次等待至多50ms检查取消，连接期限5秒、单请求期限10秒。磁盘写入、SHA-256和发布也在此线程。
- 复用纯C++ HandshakeExchange和Download编解码。现有QtTcpTransport的业务响应限制为64KiB控制帧，不改动它以适配256KiB下载数据；沿用BackupWorker的线程内socket模式。
- 协议输入和socket读缓冲有上限；一个块最多256KiB，每次只有一个请求在途。解析/校验有少量有界副本，不缓存文件正文；进度通过原子值合并，最多一个待处理进度事件。
- QObject context queued回调仅在GUI线程改状态；析构先请求取消并join，之后QObject基类销毁清理未执行事件。MainWindow正常关窗先取消并等待finished后重试关闭；直接析构先使用既有接口停止列表查询和健康连接，再销毁页面，避免页面拆卸回调访问已删除控制器，最后取消并join任务。此处只补shutdown顺序，不改变健康检查和上传策略。

## Windows 安全保存与取消

- 本地用户目标与服务器相对逻辑路径完全分离。只接受本地盘绝对路径，总长度小于240字符；明确拒绝UNC、设备路径、ADS、..、非法字符、尾随空格/点及设备保留名。
- 逐组件CreateFileW打开祖先目录，FILE_FLAG_OPEN_REPARSE_POINT配合句柄属性检查拒绝重解析点；保留不含FILE_SHARE_DELETE的目录句柄到任务结束，阻止目录重命名/替换。
- 目标同目录随机`.chunkharbor-download-<UUID>`以CREATE_NEW建立，文件句柄不共享，持有GENERIC_WRITE|DELETE。正常异常由RAII通过FileDispositionInfo删除自己的文件对象，不按可能被替换的路径删除。
- 完成校验和FlushFileBuffers后，使用SetFileInformationByHandle(FileRenameInfo)，ReplaceIfExists=FALSE。已有目标检查仅用于提前报错；真正的防覆盖保证来自这个系统操作。不会回退到替换或复制覆盖。参见[Microsoft FILE_RENAME_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_rename_info)。
- 原子phase状态：Running→Cancelled或Running→Publishing，CAS决定先后。提交点前取消必不发布；一旦进入Publishing，取消不再反转结果，最终报告系统发布的成功或失败。GUI不等磁盘锁。发布后的再次下载拒绝覆盖。
- Windows文件数据flush不等于已验证硬件掉电下目录元数据持久性。强杀进程可能留下临时文件，本轮不声明掉电恢复或下载续传。

## 验证入口

使用本机实际Kit：Qt6.8.3 / MinGW13.1，CMake位于D:/CMake/bin，Ninja沿用build-windows-gui缓存。

```powershell
$env:PATH='D:/QT/Tools/mingw1310_64/bin;D:/QT/6.8.3/mingw_64/bin;D:/CMake/bin;'+$env:PATH
cmake -S . -B build-windows-gui -DCMAKE_BUILD_TYPE=Debug `
  -DCHUNKHARBOR_BUILD_SERVER=OFF -DCHUNKHARBOR_BUILD_CLI=OFF `
  -DCHUNKHARBOR_BUILD_GUI=ON -DCHUNKHARBOR_BUILD_TESTS=ON `
  -DCMAKE_PREFIX_PATH=D:/QT/6.8.3/mingw_64
cmake --build build-windows-gui -j 2
ctest --test-dir build-windows-gui --output-on-failure
ctest --test-dir build-windows-gui -LE native-desktop --output-on-failure
python tests/integration/windows_gui_download_ssh_test.py `
  build-windows-gui/chunkharbor-gui-download-probe.exe miniob-ubuntu `
  /home/ning/ChunkHarbor/build/chunkharbor-server build-windows-gui/r2-1-download.png
python tests/integration/windows_mode_ssh_test.py `
  build-windows-gui/chunkharbor-gui-transfer-probe.exe miniob-ubuntu `
  /home/ning/ChunkHarbor/build/chunkharbor-server
```

测试用专用临时根和随机端口，不访问真实备份；未同步或修改Ubuntu源码，服务端继续使用f00f601既有协议实现。探针自动操作真实FilesPage及保存框，自动化强制Qt非原生对话框；正式GUI默认Windows原生对话框，其人工交互未在本轮验证。Linux CLI保持现有实现；Linux Qt GUI本轮未新增安全输出后端，会明确拒绝GUI下载而提示使用CLI。

自动测试包括28个TCP场景：空/普通/多块及中文路径、块及整文件摘要错误、断连、部分写入后断连、超时、取消及部分下载取消、既有目标、普通及硬链接目标竞争、junction父目录、目录替换、错误路径/版本/request/type/范围、浮点/超限大小、截断、关窗和直接销毁。每个任务还检查重复启动拒绝、GUI心跳及临时文件清理。download_output_test只在测试编译单元包装真实Win32调用，注入磁盘满、短写和flush失败，生产代码无故障开关。

## 实际结果（2026-11-04）

| 检查 | 本轮结果 | 原始证据（Windows构建目录） |
| --- | --- | --- |
| Windows配置、构建 | 退出0；Qt6.8.3、MinGW13.1.0 | r2-1-build-delivery.log |
| 最终非原生CTest | 26/26通过、136.89秒、退出0；含28个下载场景及10轮在途连接销毁 | r2-1-ctest-delivery.log |
| Windows GUI→SSH→Ubuntu下载 | 空/中文/793856字节多块，大小及SHA-256全部一致、退出0 | r2-1-ssh-download-delivery.log |
| R1真实SSH上传模式矩阵 | 12组通过、退出0 | r2-1-ssh-upload.log |
| 原生下拉框 | 首轮完整CTest中仍失败，沿用R1人工验收例外，未删除/禁用/修复 | r2-1-ctest.log |
| 差异检查 | git diff --check退出0 | 当前工作区 |

首次完整CTest为24/26，除原生下拉框外，文件列表关窗出现堆损坏；修复页面/控制器销毁顺序。随后健康检查析构崩溃由GDB定位到socket断开回调访问已销毁定时器，新短生命周期测试先复现失败，再在MainWindow关闭顺序中停止列表和健康连接后通过。失败日志r2-1-ctest-verified.log、r2-1-health-debug.log保留。早期构建并发冲突与探针argv编码错误也已修正；不以最终通过改写初次结果。

最终SSH下载专用根 `/tmp/chunkharbor-r2-1-download-5taCk4mr`，R1矩阵根 `/tmp/chunkharbor-mode-ZsXHw2YB`；测试服务和隧道已停止，根保留用于审计。服务端/Windows一致的摘要：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| empty | 0 | e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 |
| 中文文件.txt | 19 | 21f4a252ac409e4d061ba1048eb22d9541b73d8877754d8fabba69b6a4174602 |
| multi.bin | 793856 | 75e455474d8e37ed17f4963da7e66c881675218f6449b6f4b095f90104732795 |

Windows原生保存框人工交互、MSVC/Release、Linux Qt GUI下载、硬件掉电及强杀恢复未验证。磁盘故障是Win32调用注入，不是真实磁盘耗尽；未改服务端源码/schema，未重新执行Linux全量构建/CTest。独立只读审查未发现遗留阻塞问题，其结论不代替上述运行证据。保留单任务/64MiB/本地盘短路径/不覆盖/无下载续传限制。未提交或推送，下一项为R2.2历史元数据及有界查询设计，本轮不继续实现。
