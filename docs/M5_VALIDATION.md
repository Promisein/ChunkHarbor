# M5 验收记录（2026-11-02）

M5交付为Windows中文Qt Widgets真实备份客户端：后台目录扫描、服务端差异预览、固定块上传、持久续传、任务状态、本机版本记录、真实日志及设置。默认界面不再使用示例数据或QTimer模拟备份成功。

## 实际环境与结果

- Windows：Qt6.8.3 / MinGW13.1 / Ninja，build-windows-gui Debug；构建成功，完整CTest **18/18通过**。
- Ubuntu：SSH别名miniob-ubuntu，隔离源码目录`/tmp/chunkharbor-m5-HZIF3EXX`，GCC11.4；原M4服务端/CLI源码未修改，当前CMake GUI=OFF重新配置/构建，Debug **44/44通过**。
- 真实Windows Widgets窗口、TransferController、后台worker经SSH连接Ubuntu上传模式服务端，完整专项脚本通过。仅使用脚本创建的Windows临时源/状态目录和Ubuntu私有新根，不读取真实文档，不覆盖已有备份。
- 六页面截图检查正常，备份默认窗口完整显示配置、预览及进度操作；设置页可滚动。目录边界测试真实生成10000个空文件及一个排除目录，明确报超限，不静默漏行。
- 独立代码审阅发现的恢复、进度、模式、端点及计划身份问题均修复，最终静态复查无新的Important问题。

Windows命令：

```powershell
$env:PATH='D:\QT\Tools\mingw1310_64\bin;D:\QT\6.8.3\mingw_64\bin;'+$env:PATH
cmake --build build-windows-gui -j 2
ctest --test-dir build-windows-gui --output-on-failure
python -X utf8 tests/integration/windows_gui_ssh_test.py `
  build-windows-gui/chunkharbor-gui-transfer-probe.exe miniob-ubuntu `
  /tmp/chunkharbor-m5-HZIF3EXX/build/chunkharbor-server
```

Ubuntu重跑沿用独立目录的依赖缓存：

```bash
cd /tmp/chunkharbor-m5-HZIF3EXX
cmake -S . -B build -DCHUNKHARBOR_BUILD_GUI=OFF
cmake --build build -j 2
ctest --test-dir build --output-on-failure
```

初次配置使用M4_VALIDATION.md相同的SQLite/OpenSSL开发头及库路径；上述隔离目录已保留缓存，不安装或修改系统软件。

## 覆盖范围

| 场景 | 实际结果 |
| --- | --- |
| 只读预览 | 真实Added/Modified/Unchanged计划，正式文件尚未生成 |
| 首次目录备份 | 中文文件名/本地目录、空文件、排除build；正式文件SHA-256与Windows一致 |
| 无变化 | 重复备份各文件变化块数0，不发送DATA_CHUNK |
| 单块变化 | 修改一个字节只变化1块，最终SHA一致 |
| 预览后源变化 | 拒绝该文件上传，远端已有摘要不变，任务不报全成功 |
| 预览后基线变化 | 另一GUI实例发布新版本，旧计划失败，较新远端内容保持完整 |
| full保护 | 已有目标拒绝首次完整上传 |
| 崩溃恢复 | 实际Windows进程kill及Ubuntu SIGKILL；原根重启、原源重开恢复，最终SHA一致 |
| 暂停/取消/Resume | 持久确认后请求暂停并取消，重开Resume完成并校验SHA |
| 优雅关窗 | 上传中调用窗口关闭、协作取消，重开恢复完成并校验SHA |
| 丢失本地完成标记 | Fake TCP保留COMMITTED，人工回退本地完成标记；经核心核验后恢复历史。这是确定性状态模拟，不是硬件掉电 |
| full→resume | Fake TCP保留原full语义，仅补缺失块 |
| 恶意响应 | 错误request_id、plan session/base拒绝，错误计划不持久化；保留原握手七类回归 |
| 本地状态 | 原子写失败、源消失/变化、暂停中取消、UTF8、64位JSON字符串精度、损坏历史和提交去重 |
| GUI展示 | 空状态无示例，真实历史/版本选择、日志组合筛选、设置重开、端点修改失效旧在线状态 |

专项探针是测试用可执行文件：创建真实MainWindow、页面、模型与TransferController，使用同一worker；脚本驱动控制器及关窗事件，不使用CLI上传替代。实际按钮体验步骤见BUILD_WINDOWS.md。事件循环heartbeat在后台任务期间持续更新，不据此宣称性能指标。

成功专项测试之一的数据目录为`/tmp/chunkharbor-m5-data-Sjzm28UJ`，本地记录在`%TEMP%/chunkharbor-m5-gui-rqzthiot`；脚本最终停止自己启动的服务与隧道，保留证据，不删除真实数据。

## 发现并修复的失败

- 独立审阅指出COMMITTED响应丢失后基线与旧checkpoint不一致、full转Resume身份不一致、续传分母不准、端口改动仍显示旧在线；补回归后修复。
- 计划统计必须绑定可信checkpoint；恶意session/base计划不允许先写状态再被核心拒绝。已观察RED并修复验证顺序。
- 真实SSH恢复曾失败：`MALFORMED_MESSAGE: request_id must increase and be nonzero`。原因是预查询大request_id后核心从2开始，已改为分开的查询/恢复/正式传输连接，Fake服务也检查递增，最终真实联调通过。
- 原1MiB选项超过DATA_CHUNK净数据上限，新增回归先失败，后改为64/256/512KiB。
- 上限测试初稿沿用chunkSize=0而失败，显式恢复有效块大小后才验证到真实10001条目边界。
- 独立测试和构建并发曾占用Windows exe、运行旧测试程序；停止并发后串行构建/测试通过，不将中间失败记作成功。
- 原probe入口会保存临时端口，现隔离QSettings/历史/日志到临时目录；重跑七类probe后用户端口保持18787。

## 边界与未执行

未运行Linux GUI、MSVC、ASan/Release新增GUI测试、物理磁盘满/硬件掉电或性能基准。Linux Debug与Windows联调不代表这些场景通过。

文件/版本页仅为本设备已核验成功提交记录（最多500条），不是服务端全量分页目录。M6下载恢复/并发性能、M7 TLS/认证/正式安装包未实现，恢复/凭据控件禁用。单文件64MiB、目录10000条目、单通道、固定SHA-256，继续使用可信loopback/SSH。

本轮工作在codex/m5-gui分支，保留前轮未提交UI；没有提交、推送或合并。
