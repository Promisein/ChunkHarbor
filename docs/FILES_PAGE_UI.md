# 文件页面 UI 重构（2026-11-04）

本轮为独立 UI 任务，不推进 R2.2。开始时 HEAD 之外已有 9 个修改文件和 11 个未跟踪文件（包括 R2.1 下载实现、探针及验收记录），暂存区为空；这些工作均保留，未 reset/clean/commit/push。

## 源码核对与改动

- 原 FilesPage 的 InfoCard 和详情纵向堆叠；QPlainTextEdit 有 150–220px 高度约束，详情还容纳多个下载控件。MainWindow 对所有页面套 QScrollArea，导致页面滚动和表格滚动叠加。表格本身没有固定小高度，主要问题是纵向空间分配。
- 复用 WorkspacePage、InfoCard、ServerFilesModel、FileListController、DownloadController 和 Light.qss。仅文件页跳过外部 QScrollArea，其余页面的业务与滚动结构保留。
- 主导航 188px；文件页为横向 QSplitter，左侧表格伸展、42px 行高，右侧默认约 320px、最小 280px，可拖动并通过“详情”按钮收起。默认 1280×820，最小窗口沿用 1060×700。
- FilePresentationModel 是页面内的 QSortFilterProxyModel：保留原始四列和整数数据，只隐藏版本/摘要列并格式化大小。大小显示使用 1024 进位，精确字节仍在原模型和 Tooltip 中；未改协议、JSON 数字或元数据。
- 搜索明确只筛选当前页；刷新在顶部，分页在底部。FileListController 仅增加 pageNumber 展示计数：成功 Refresh/Next/Previous 后更新，失败和取消不变；端点更改重置。不从记录数推算总页数，128 页游标历史截断不影响显示序号。
- 详情 QLabel 使用 PlainText，长名称、路径、版本和哈希按当前宽度省略；Tooltip 做 HTML 转义；复制使用 fullText 原值。过滤及 modelReset 后同步刷新详情，失效选中项禁用下载，避免旧条目残留。
- FilesPage::transferBar() 暴露表现层 Widget，MainWindow 将其加入右侧内容布局并重新设置 QObject 父级，因此页面切换后仍可查看进度与取消。独立使用 FilesPage 时传输栏留在页面底部。
- UI 通过原 DownloadController::changed 读取 fileName/status/received/total，取消直接调用 cancel；选中行不参与任务状态展示。保存框前仍复制选中文件元数据与端点。未改 DownloadController、DownloadOutput、SHA-256、上传、协议、服务端、schema 或关窗清理语义。

## C++ / Qt 取舍

QSplitter 与 Expanding QSizePolicy 把额外空间交给表格，伸展项只用于将下载按钮推到详情底部；不以绝对坐标布局。QObject 父子关系管理 Widget，带接收者上下文的信号连接随页面销毁自动断开。下载控制器仍归 MainWindow 持有，后台线程和 queued 状态通知复用 R2.1；视图先于控制器销毁的既有顺序保持。

## 本轮验证

真实命令、结果与未验证项见 PROGRESS 顶部“文件页面独立 UI 重构”。测试数据均位于 QTemporaryDir / Python TemporaryDirectory；截图使用人工构造的隔离布局夹具，不是服务器真实备份，不作为下载成功证据。

测试修改：gui_pages_test 增加布局伸展、无页面嵌套滚动、列隐藏、真实详情、完整哈希复制、过滤清理和面板收起断言；加载真实 QSS。gui_files_probe 改为读取 QLabel 原始值，并验证成功翻页和失败保留的页码。gui_download_probe/test 增加在下载中更换选中项并切换页面，仍完成原文件下载的真实 TCP 场景；原校验失败、取消、目标保护及销毁场景保留。

## 本轮文件清单（含在既有未提交修改上继续编辑的文件）

- `apps/gui/pages/FilesPage.cpp`、`apps/gui/pages/FilesPage.h`
- `apps/gui/MainWindow.cpp`
- `apps/gui/FileListController.cpp`、`apps/gui/FileListController.h`
- `apps/gui/styles/Light.qss`
- `CMakeLists.txt`（页面测试加载 QSS）
- `tests/unit/gui_pages_test.cpp`
- `tests/integration/gui_files_probe.cpp`、`tests/integration/gui_files_test.py`
- `tests/integration/gui_download_probe.cpp`、`tests/integration/gui_download_test.py`
- `docs/FILES_PAGE_UI.md`、`docs/PROGRESS.md`

首轮旧布局回归明确失败于版本/哈希列仍可见；首次新夹具因路径未按字节序排序被既有协议校验拒绝，已修正夹具而未放宽校验。首次非原生 CTest 为 25/26：不支持 capability 的模拟端在首个 10ms 心跳前拒绝，心跳为零；该夹具补充 40ms 延迟，与其他列表场景一致，保留原心跳断言。最终结果另见 PROGRESS；早期失败日志保留。

原生 Windows 平台与 offscreen 平台分别检查；offscreen 的字体渲染不代表 Windows 实际显示，视觉核对使用 windows 平台截图。DPI 检查以 Qt 逻辑像素尺寸为准，物理屏幕可用尺寸低于既有 1060×700 最小窗口时，不能将最大化的小型虚拟屏当作正常桌面尺寸。
