# 中断后重新预览与备份页布局修复（2026-11-02）

## 根因与边界

用户日志实际为12项中5个CONFLICT、7个新增，并非512项错误。只读核对用户本地春熙路checkpoint：5条completed=true、mode=full；只读SQLite查询：5个current_files和5个COMMITTED上传会话。没有删除或改写这些数据。

调用链：BackupPage扫描→TransferController::scan清空模型/固定当前配置→后台runBackup→full设置BaseSelector::Absent→即使本任务已经有completed记录，预览仍发送“目标不存在”前置条件→服务端拒绝已提交文件。目录切换使问题显现，但已完成A直接重新扫描同样能复现。TransferController busy门禁、工作线程join和同QObject queued回调先后关系未发现旧回调污染证据，因此不无理由重写线程生命周期。

修复只放宽**预览**：完整本地源/根目录/逻辑路径/端点/块大小/摘要/模式匹配，且已有本任务completed记录（或丢失回执已由runDelta核验COMMITTED）时，允许Auto查询。已有版本必须为Unchanged；若远端已变化仍拒绝。正式上传仍绑定预览的Exact基线，外部首次full上传依旧拒绝已有目标。

未完成会话预检按当前根目录、端点及映射后的逻辑路径隔离，不同远端前缀不会因旧源会话报“源缺失”。同一目标的源内容变化继续保护性拒绝，提示恢复原源/配置或使用新远端路径；不删除checkpoint或服务端会话，不盲续。测试验证新前缀安全重传后，恢复原内容仍可继续旧会话。

这不是“自动放弃旧会话并覆盖同一目标”的实现；当前没有显式放弃/替换会话协议，本轮没有为此改协议或服务端行为。

## 布局

截图中的下方列表实际是变更预览QTableView，不是日志页文本框。BackupPage给外层preview卡片stretch=1，但表格maximumHeight=190、内部stretch=0。最大化时表格无法扩展，标题和统计QLabel分得多余高度，形成空白。保留样式和原最小高度，仅移除最大高度限制并将表格addWidget stretch设为1。InfoCard、其他页面、字体与配色未改。

## 验证

Windows Qt6.8.3/MinGW13.1/Ninja，使用原build-windows-gui：

```powershell
$env:PATH='D:/QT/Tools/mingw1310_64/bin;D:/QT/6.8.3/mingw_64/bin;'+$env:PATH
cmake --build build-windows-gui -j 2
ctest --test-dir build-windows-gui --output-on-failure
$env:QT_QPA_PLATFORM='windows'
./build-windows-gui/chunkharbor-backup-layout-test.exe
python -X utf8 tests/integration/windows_gui_rescan_test.py `
  build-windows-gui/chunkharbor-gui-transfer-probe.exe miniob-ubuntu `
  /home/ning/ChunkHarbor/build/chunkharbor-server
```

- 行为红测：真实GUI controller/worker完成full后再预览，旧代码两条已完成文件均CONFLICT；布局红测card=339/table=190，标题高度断言失败。
- 修复后完整CTest22/22通过（59.96秒，非性能基准）。
- 原生Windows平台真实窗口几何测试通过：0/3/200行，普通/放大/最大化/还原/缩小、末行滚动可达。原生普通窗口table=225，大尺寸505，最大化486；标题/统计/表格间距始终8。offscreen同样通过。不声称人工鼠标拖拽或全DPI截图矩阵已验收。
- SSH专项使用Ubuntu原工程服务端，临时数据仅在其build下独立rescan-data目录；源为Windows TemporaryDirectory，未使用用户照片。覆盖：完整上传再预览、真实持久ACK后取消再预览、同一controller A→B→A、B提交后A续传、重复扫描busy门禁、扫描启动后取消重扫、源改变拒绝盲续/新前缀安全重传/恢复原源继续；最终SHA-256一致。另验证外部full/远端变化保护、空目录、缺失目录、Windows独占句柄导致不可读与释放后恢复。
- 新增脚本结束后只清理自己的测试根，不清理用户数据或有效用户会话。
- 独立只读审查未发现新增Critical/Important，审查者未重复运行测试。

失败记录：一轮构建与旧测试运行重叠导致输出exe占用，待旧测试结束后顺序重建成功。取消夹具最初可在首个小文件后取消、没有未完成大文件会话，改为大文件持久ACK后取消。空目录夹具曾与测试workspace重名而包含日志，分离源/状态目录后重跑。这些失败不计成功。

本轮修改：BackupWorker.cpp、BackupPage.cpp、CMakeLists.txt、gui_transfer_probe.cpp，新增backup_layout_test.cpp与windows_gui_rescan_test.py，以及本记录/PROGRESS。未改网络协议、SQLite schema或持久化格式；未改服务端。未运行Linux GUI、MSVC、Release、Sanitizer、真实硬件掉电/网络故障矩阵；未修改用户现有会话。现有CLion运行窗口仍为旧进程，应关闭后运行新build-windows-gui产物或重新构建CLion配置。未提交/推送。
