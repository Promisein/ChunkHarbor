# UI-01 版本历史三栏与浏览页视觉规范（2026-11-04）

## 范围与现状核对

本轮为独立 UI 任务，不推进服务器历史恢复。保留此前 R2.1、文件页及文档的全部未提交修改，无 reset/clean/commit/push。实际数据来自 WorkspaceStore::records()，不是服务端历史查询；存储按端点和版本 ID 去重，最多500条。文件按“端点＋逻辑路径”分组；原实现版本表格展示5列并使用字符串时间排序，下方详情展示 SHA 与上传计划说明；外部 QScrollArea 与内部列表叠加。

BackupWorker 在成功提交时写 UTC ISODateWithMs；changedBlocks/missingBytes 表示提交时上传计划中的实际变化块数/字节数，不表示重新比较任意两版，也不表示文件完整大小。versionId 是版本标识，baseVersion 是基线版本，sha256 是内容摘要；本轮分别准确标注。

## 布局与绑定

- 横向 QSplitter：本机备份文件（约260px）→弹性历史列表→详情（约320px）。较窄时可手动收起右栏；窗口最小尺寸沿用1060×700。页面不再包裹主窗口 QScrollArea，列表各自独立滚动。
- 本机文件筛选不发网络请求。文件行42px，长中文路径省略，Tooltip 显示完整路径与端点，禁用水平滚动。相同逻辑路径在不同端点仍分别列出。
- 历史使用 QListView 和轻量 delegate，三行自然展示本地时间、变化类型/大小、短 ID；QStandardItemModel 仍保留五列及每行完整 FileRecord，不丢失变化块字节、基线或摘要。
- 详情仅由选中行的完整记录驱动；切换文件、筛选无匹配、无效版本选择均清理旧字段并禁用复制。刷新本机记录时保留文件选择，版本仍默认最新一条。
- 时间解析为 QDateTime 后 toLocalTime()，显示 yyyy-MM-dd HH:mm；原 ISO 字符串完整保留在模型及 Tooltip。排序比较真实时刻，修正带不同时区偏移及同秒毫秒的字符串排序风险，相同时刻稳定排序；缺失时区的旧记录显示“时间或时区未知”，不擅自假定UTC。
- 基线 absent 显示“无（首次提交）”且禁用复制；真实版本/基线/内容SHA各自可复制完整值。旧禁用恢复按钮改为“历史版本恢复将在 M6 阶段提供”提示；项目当前路线仍以 R2 排期，不据该历史阶段提示宣称已实现恢复。
- 继续使用主窗口现有传输栏，未新建任务、下载或恢复控制器。

## 两页共用规范

新增 header-only `widgets/BrowserUi.h`：共享 browserPage / browserSplitter / browserList / detailPanel / detailField / browserEmptyState 和 ElidedLabel。QObject 父子关系持有 Widget，连接带所属面板/页面上下文；没有工作线程或网络资源变化。ElidedLabel 的 fullText 独立于可视省略文本，Tooltip 转义服务端/本地字符串。

Light.qss 仅按 visualRole/role 定义统一列表边框和浅蓝选中、详情圆角、复制按钮、空状态与字体层级；删除 FilesPage 专属的重复视觉选择器。FilesPage 复用这些工具与样式，并增加同样的空列表/无匹配状态，原下载按钮、保存框、数据传输、安全输出、SHA及取消不变。

## 本轮文件清单

- apps/gui/pages/VersionsPage.cpp
- apps/gui/pages/FilesPage.cpp
- apps/gui/MainWindow.cpp
- apps/gui/widgets/BrowserUi.h（新增）
- apps/gui/styles/Light.qss
- CMakeLists.txt
- tests/unit/gui_pages_test.cpp
- tests/unit/versions_page_test.cpp（新增）
- README.md
- docs/PROGRESS.md
- docs/UI_01_VERSIONS.md（新增）

配置、构建、测试、截图与未验证边界见 PROGRESS 顶部本轮条目。测试全部使用隔离 QTemporaryDir，本机真实历史文件不读写；截图为人工构造的布局测试记录。
