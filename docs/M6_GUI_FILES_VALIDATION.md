# M6 Windows GUI文件分页验收（2026-11-02）

本轮仅接真实服务端列表；GUI下载按钮仍禁用。设计与文件映射见[M6_GUI_FILES.md](M6_GUI_FILES.md)。Linux CLI下载及服务端列表本身的历史结果见对应M6验收文档，不与本轮GUI结果混同。

## 使用

启动包含M6列表功能的Ubuntu服务端和SSH本地转发，在GUI“设置”选择转发端口，然后进入“文件”点击“刷新列表”。顶部“检查连接”只是连接检测，列表使用独立连接；列表成功不伪造顶部持久在线状态。

- “上一页/下一页”按实时路径游标查询，每页最多32条；下一页可能因长路径提前截页。上一页保留最近128个游标，刷新回到开头。
- 搜索只筛选当前页；不显示未知的总文件数。详情可选择复制，长内容可滚动。
- 失败保留上次结果并标注；修改端口清空旧数据。旧服务端未协商file_list_v1时提示升级，不回退为本机历史。
- “版本历史”仍展示本机记录，与当前服务器目录含义不同。GUI恢复尚未实现。

## Windows实际命令与结果

环境Qt6.8.3 / MinGW13.1 / Ninja，build-windows-gui Debug：

```powershell
$env:PATH='D:/QT/Tools/mingw1310_64/bin;D:/QT/6.8.3/mingw_64/bin;'+$env:PATH
cmake --build build-windows-gui -j 2
ctest --test-dir build-windows-gui --output-on-failure
```

最终构建成功，完整 **21/21通过**，54.85秒，不作为性能测量。包含原gui_worker_test与7类gui_probe握手回归。windeployqt既有dxcompiler/dxil缺失警告仍存在，非本轮正式安装包验收。

gui_files_test使用真实MainWindow、文件页、模型、控制器和QtTcpTransport，与loopback测试服务端交互，覆盖21类：

- 首/次/上一页、中文当前页筛选，路径包含HTML标记仍按纯文本展示，tooltip转义，2^53以上大小精确显示。
- 空列表、下一页失败保留旧页与游标、跨游标不前进、缺能力、重复路径、错误游标、条目超过请求32条。
- 错误request_id/类型、畸形JSON、ERROR/BUSY、超限帧头、尾随数据、异常断连、截断帧。
- 取消、端点切换、查询中销毁窗口、端点切换后立即重查，旧结果不混入；真实10秒超时。计时器heartbeat持续执行，仅证明异步测试时事件循环仍运行，不宣称延迟性能指标。

## 真实Windows→SSH→Ubuntu

最终执行：

```powershell
python -X utf8 tests/integration/windows_gui_files_ssh_test.py `
  build-windows-gui/chunkharbor-gui-files-probe.exe miniob-ubuntu `
  /tmp/chunkharbor-m6-gui-list-SmsSMQdt/build/chunkharbor-server `
  build/m6-gui-files.png
```

脚本成功退出0：专用根`/tmp/chunkharbor-m6-gui-files-data-UKda26Zy`，服务启动后用真实Linux CLI upload提交71个文件（含中文和二进制数据）。真实Qt文件页返回首32条、下一32条和重新查询首32条；当前页中文筛选为1条，路径、精确大小和SHA-256元数据与源数据逐项一致。服务及隧道均由脚本停止，根目录保留，仅为专用测试数据。

提供截图路径时使用原生Windows QPA、正式样式qrc及与main相同的Fusion浅色palette；无截图时可offscreen。原生截图已检查中文、按钮、表格和详情可视高度。截图为build下忽略产物，不进入Git。

## Linux无Qt边界回归

新建源码隔离目录`/tmp/chunkharbor-m6-gui-list-SmsSMQdt`，使用本轮源码，实际执行：

```bash
cmake -S /tmp/chunkharbor-m6-gui-list-SmsSMQdt \
  -B /tmp/chunkharbor-m6-gui-list-SmsSMQdt/build \
  -DCMAKE_BUILD_TYPE=Debug -DCHUNKHARBOR_BUILD_GUI=OFF \
  -DSQLite3_INCLUDE_DIR=/home/ning/ChunkHarbor/build/deps/sqlite/usr/include \
  -DSQLite3_LIBRARY=/usr/lib/x86_64-linux-gnu/libsqlite3.so.0 \
  -DOPENSSL_INCLUDE_DIR=/home/ning/ChunkHarbor/build/deps/openssl/usr/include \
  -DOPENSSL_CRYPTO_LIBRARY=/usr/lib/x86_64-linux-gnu/libcrypto.so.3 \
  -DCMAKE_CXX_FLAGS=-I/home/ning/ChunkHarbor/build/deps/openssl/usr/include/x86_64-linux-gnu
cmake --build /tmp/chunkharbor-m6-gui-list-SmsSMQdt/build -j 2
ctest --test-dir /tmp/chunkharbor-m6-gui-list-SmsSMQdt/build --output-on-failure
```

配置/构建成功，完整 **50/50通过**，31.50秒。configure.log/build.log/ctest.log保留于隔离根。后续仅GUI和GUI测试/文档修正，未修改Linux服务端或纯C++协议；不声称Linux GUI已构建。

## 失败、审查和边界

先修改旧页面测试，真实观察到本机历史错误填入服务器列表的红测。close场景首次因最后窗口自动关闭导致退出0但无探针结果，修正测试app退出策略后通过。offscreen截图字体缺失，不能作为文字排版证据；改原生Windows检查。原生结果输出曾截断于4096字节，显式flush后通过；详情高度检查曾发现QLabel压缩裁切，切换只读可滚动QPlainTextEdit后通过。

独立只读审查发现ToolTipRole自动富文本解释的P2问题，整体转义tooltip、分离纯文本详情角色并增加断言后关闭；复审无剩余Critical/Important。审查者未重复运行测试。

未运行Linux GUI、MSVC、Release、ASan/UBSan/TSan、新的完整GUI上传端到端、GUI恢复、性能或大规模并发实验。当前列表查询不重新哈希正文；分页为实时视图，不保证跨页快照或总数。历史游标最多128个，当前页32条，不积累文件全集；不将此静态资源界限称为压力测试结论。

Ubuntu主工程及用户备份未修改；测试路径私有。需使用包含M6列表的服务端，部署中的旧二进制不会自动更新。本轮未提交或推送。
