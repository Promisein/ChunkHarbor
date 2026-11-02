# M5 Windows Widgets客户端

## M6服务端文件分页（2026-11-02）

文件页已切换为真实服务端当前文件，版本历史页仍为本机记录。新增ServerFilesModel（QAbstractTableModel）与FileListController，复用QtTcpTransport新增的异步请求模式；每页独立协商file_list_v1，单次在途请求，5秒握手/10秒响应、64KiB接收界限。控制器检查页大小、顺序与游标，成功才替换当前页；失败保留并标注上次结果，取消/端点切换中止旧连接，端点变化清空模型。所有socket/model/timer归Qt事件线程，无阻塞磁盘/网络等待。

每页32条，128个历史游标，提供刷新/翻页/取消及当前页搜索；大小显示精确整数。远端字段在详情使用只读可滚动QPlainTextEdit，悬停提示转义后展示，避免路径解释为HTML。恢复按钮保持禁用，GUI下载是下一独立任务。MainWindow仅组装控制器/页面并转发设置端点，不处理协议。

Windows完整21/21、Linux GUI=OFF50/50、真实Windows原生窗口→SSH→专用Ubuntu服务端71文件分页均通过；详见M6_GUI_FILES.md与M6_GUI_FILES_VALIDATION.md。以下M5与2026-11-01内容作为历史设计记录，其“文件页为本机历史”等表述不代表当前状态。

## 下拉框与进度显示（2026-11-02）

全部QComboBox统一由widgets/ComboBox.h提供QListView及QStyledItemDelegate，避免平台菜单delegate的方形框与默认箭头；样式和SVG箭头在styles资源内统一维护。备份进度保留8px细条，百分比由上方独立标签显示，避免将文字画在8px轨道内。Windows构建及18/18回归通过，125%/200%截图检查0%、50%、100%可读。仅显示层调整，不改变实际确认字节和传输语义。

## M5真实业务接入（2026-11-02）

默认六页已移除示例数据与定时器模拟进度。MainWindow只组装页面和连接状态；TransferController在GUI线程拥有TaskModel（QAbstractTableModel）、任务互斥和后台线程生命周期。BackupWorker在独占后台线程创建、使用和销毁QTcpSocket及源文件句柄，复用protocol/client_core的DirectoryScanner、SourceSnapshot和runDelta；扫描、读文件、哈希、同步网络等待不会在GUI线程运行。

后台通过绑定QObject上下文的queued调用更新模型，进度至少间隔100ms（阶段/终态可即时通知），最多10000个扫描条目（包含排除/错误）。窗口关闭时先协作取消，后台结束后再关闭；析构join保证无悬空回调。Socket等待按100ms轮询取消并采用总超时，暂停在安全操作边界生效。

备份前真实预览，执行时固定源SHA-256和服务器基线；源变化、目录变化或外部新版本会拒绝旧计划。full模式保护已有目标，incremental按固定块复用，resume继承保存会话语义。原子checkpoint绑定本地路径、逻辑路径、根目录、端点、块大小、摘要和模式；整数以十进制字符串存储。QUERY_DELTA查询缺失块；提交响应丢失时核验原会话COMMITTED结果，恢复本地历史后再预览。独立协议连接避免请求ID回退，恢复历史不伪造计划统计。

WorkspaceStore保存最多500条本机真实版本和500条活动日志，小型元数据使用QSaveFile；非秘密偏好使用QSettings。文件/版本页为本设备提交历史视图，明确不代表服务器全量目录，不提供M6恢复能力。日志可组合筛选/导出，设置支持端口、64/256/512KiB块、续传、排除目录名、启动页及日志目录；SHA256、单通道、浅色为当前固定能力。认证占位禁用。

新增文件：TransferTypes.h、TaskModel.h/.cpp、WorkspaceStore.h/.cpp、TransferController.h/.cpp、BackupWorker.cpp。原六页面切换到真实模型与记录；QtTcpTransport保留异步连接探测，修改端口立即失效旧在线状态。内部Qt库只在GUI开关开启时编译，Linux服务端和共享纯C++库不引入Qt依赖。

测试与真实结果见M5_VALIDATION.md；CLion/PowerShell启动、SSH与手工验证见BUILD_WINDOWS.md。以下2026-11-01内容保留为前轮开发历史，其中“示例/原型未接入”的描述不代表当前实现。

## 简体中文界面（2026-11-01）

六页导航、顶部状态、按钮、表头、空状态、无障碍名称、示例数据、演示进度、日志筛选/导出及握手错误提示已统一中文。品牌名ChunkHarbor、协议标识、摘要、地址和技术单位保留原样；命令行的`--page backup`等稳定参数不变。界面继续明确区分“演示”“示例”和真实握手状态。

应用默认QLocale为简体中文，Qt自带qtbase_zh_CN.qm从当前Qt Kit嵌入资源，通过QTranslator加载；Qt标准控件翻译不依赖部署目录或运行PATH。若其他Qt Kit未安装该翻译文件，CMake给出警告，应用自有文案仍为中文，标准对话框回退系统默认。字体优先Microsoft YaHei UI。为GUI及页面测试的MSVC目标补充/utf-8，但本轮未实测MSVC。

本机Windows Qt6.8.3/MinGW13.1构建成功，完整CTest16/16通过；页面测试的中文状态、日志筛选及版本联动断言通过，旧握手七场景保持通过。六页以`--page NAME --screenshot PATH`渲染检查，未测试Linux GUI/MSVC或真实GUI备份。未提交/推送。

在CLion重新加载CMake并重新构建`chunkharbor-gui | Windows-GUI`即可看到中文，无需额外设置语言或安装翻译文件。

## 第二轮：Backup / Versions / Logs（2026-11-01）

本轮在第一轮骨架上细化三个页面，尚未连接备份/版本/日志业务。每页明确标注Demo或Sample；真实握手状态仍来自QtTcpTransport，不会被演示进度更改。

- `pages/BackupPage.h/.cpp`：本地目录选择（只选择，不扫描）、远端逻辑路径、三个备份模式、示例扫描、QTableView变化预览、进度/阶段/速度/字节/剩余时间、演示开始/暂停/继续/取消/重试。QTimer只模拟界面，不读取选中目录或发起网络请求。默认窗口能显示配置、预览和进度；较小尺寸仍可滚动。
- `pages/VersionsPage.h/.cpp`：左侧三个示例文件，右侧三行历史版本；切换文件/版本联动SHA-256和比较说明，恢复按钮禁用。摘要由固定示例种子生成，不表示真实文件校验结果。
- `pages/LogsPage.h/.cpp`：五条示例日志，Level/Time Range/搜索组合过滤；Export sample logs通过QSaveFile原子导出当前可见日志。只有用户选定导出位置才写文件，文件头标注示例来源。
- `demo/DemoData.h/.cpp`：集中维护示例文件、版本和日志；只使用小型内存数据。
- `widgets/TableKit.h/.cpp`：统一只读、行选择、表头、交替底色和状态配色；数据使用QStandardItemModel（继承QAbstractItemModel），后续可替换真实模型。
- `InfoCard::setSubtitle`：统一卡片副标题，重复调用更新同一label。样式扩展于`styles/Light.qss`，所有窗口仍采用布局管理器。

生命周期：模型、控件、定时器均由Qt父对象管理；关闭Backup页面时定时器销毁。业务逻辑尚未新增，不把示例速度/进度或版本当作服务端真实结果。下一轮仍需单独设计/接入TransferController及真实任务模型。

Windows Qt6.8.3/MinGW13.1/Ninja实际执行`cmake --build build-windows-gui -j 2`、`ctest --test-dir build-windows-gui --output-on-failure`，最终**16/16通过**。新增gui_pages_test检查演示开始/暂停/取消/重试/定时完成、文件选择更新版本摘要、日志组合过滤；保留gui_probe七类握手回归。三页通过`--page NAME --screenshot PATH`真实截图检查。未测试Linux GUI、MSVC、GUI上传；未提交/推送。

手工体验：重新构建CLion的chunkharbor-gui，打开Backup点击Start demo backup，尝试Pause/Resume、Cancel/Retry；Versions选择另一文件与历史行；Logs选择Warning或搜索checksum，导出示例日志。无需服务端。

以下为第一轮结构和历史验证记录。

本轮按用户指定范围实现MainWindow、侧栏、顶部栏、六个独立页面空壳与统一浅色样式。Dashboard仅布局卡片与空状态，不填造真实备份统计；Backup/Files/Versions/Logs业务控件与模型下一轮接入。Settings保留已有真实异步握手入口，其余偏好只作说明。

## 文件结构

```text
apps/gui/
  MainWindow.h/.cpp           导航、顶部状态和页面组装
  main.cpp                   应用浅色palette、启动参数
  QtTcpTransport.h/.cpp      现有异步握手，不涉及上传
  pages/
    WorkspacePage.h/.cpp     共用页标题、说明、内容布局
    DashboardPage.h/.cpp
    BackupPage.h/.cpp
    FilesPage.h/.cpp
    VersionsPage.h/.cpp
    LogsPage.h/.cpp
    SettingsPage.h/.cpp
  widgets/InfoCard.h/.cpp     白底圆角、轻微阴影、内容布局
  styles/Light.qss            统一字体、导航、按钮与卡片
  styles/styles.qrc           打包样式，不依赖启动工作目录
```

页面由QStackedWidget及QScrollArea管理，QListWidget导航支持键盘选择；最小窗口1060×700，默认1280×820，逻辑像素随系统DPI缩放。使用布局管理器，不设业务控件绝对坐标。Windows暗色系统下显式设置应用浅色palette及页面背景，防止内容区继承暗色。

所有页面/控件经Qt父子所有权管理；MainWindow负责组装，socket/timer仍归QtTcpTransport，状态经signal更新，未新增阻塞磁盘/扫描/哈希。后续任务数据应接QAbstractTableModel和TransferController，不能放进窗口业务代码。

## 本轮验证

Windows运行时部署开关：在CLion的CMake选项追加`-DCHUNKHARBOR_DEPLOY_GUI=ON`，重新加载并构建`chunkharbor-gui`。构建后自动运行当前Qt Kit的windeployqt，将Qt DLL、平台插件和编译器运行库复制到exe旁。此开关默认OFF，仅作用于Windows GUI，改变Qt Kit时应重置windeployqt缓存路径或使用新的构建目录。无需在运行配置手工添加Qt PATH；这只是开发运行部署，不代表完成M7打包。

实际验证：`cmake -S . -B build-windows-gui -DCHUNKHARBOR_DEPLOY_GUI=ON`、`cmake --build build-windows-gui --target chunkharbor-gui -j 2`成功；清除Qt插件环境变量、PATH仅保留Windows/System32，并从独立临时工作目录启动exe截图，退出0且截图存在。此验证不依赖Qt/MinGW的PATH，也不依赖仓库为工作目录。

Windows Qt6.8.3 / MinGW13.1 / Ninja，沿用build-windows-gui真实缓存：

```powershell
$env:PATH='D:\QT\Tools\mingw1310_64\bin;D:\QT\6.8.3\mingw_64\bin;'+$env:PATH
cmake --build build-windows-gui -j 2
ctest --test-dir build-windows-gui --output-on-failure
.\build-windows-gui\chunkharbor-gui.exe
```

可用`--page dashboard|backup|files|versions|logs|settings`指定初始页面，`--screenshot ABSOLUTE_PATH`截图并退出。保留`--probe-port N`的既有行为（成功0/失败2）。完整CTest 15/15通过，其中gui_probe含分片ACK、错误request_id、布尔类型、断连、超限、尾随数据和超时7种场景。截图检查Dashboard和Settings，另启动四个页面确认可渲染退出。

本轮未验证Linux GUI、MSVC、GUI上传或真实备份功能。Online只在真实握手成功后显示，断连回Offline；Dashboard的业务统计为明确空状态。未提交/推送。

## 前轮连接原型记录

# Qt 最小连接原型（M2）

提供地址、端口、连接按钮和状态提示，默认 127.0.0.1:18787。普通启动不自动连接。仅协商 handshake 能力，无上传、扫描、任务表和恢复。完整 GUI 留在 M5，上传仍使用 Linux CLI。

MainWindow 仅组装 UI，QtTcpTransport 独立持有 QTcpSocket 和单次定时器，在所属 GUI 线程异步收发。复用纯 C++ FrameCodec 和 HandshakeExchange；接收累计上限 4096 字节 payload 加 32 字节帧头，提前拒绝超限长度和同批额外数据。连接与握手总限时 5 秒，错误和断连显示状态。QObject 父子所有权管理 socket、timer 和窗口生命周期。无主线程磁盘、哈希或阻塞网络调用。

--probe-port N 使用同一窗口及正常 transport 自动连接本地服务器，成功退出 0，错误或超时退出 2，用于集成测试。--screenshot PATH 显示后保存截图并退出。临时本地假服务覆盖分片 ACK、错误 request_id、布尔类型错误、断连、超限、尾随字节和超时。实际构建验证记录见 docs/PROGRESS.md。

后续先从 Linux CLI 抽取纯 C++ 任务状态到 client_core，再添加 Qt 上传适配。M5 使用任务模型，明确区分 sent、durable confirmed 与 committed；后台扫描/哈希通过 queued signal 返回，socket 留在所属 QObject 线程。Windows 本地路径与相对 UTF-8 逻辑路径分离。Linux Reactor 和 UploadStorage 不进入 Windows 构建。开发仅使用本地或 SSH 隧道。
