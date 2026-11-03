# R1：ComboBox 弹层圆角与矩形底框（2026-11-03）

> 阶段收尾补充：用户已自行验证并确认下拉框没有实际故障，明确要求关闭R1。此人工验收作为本项完成依据；用户未提供具体键盘/DPI测试组合，不扩写覆盖范围。最新原生自动输入专项失败仍保留，不删除测试、不将其写成通过，也未确认失败就是鼠标操作干扰。按用户决定不再将该测试问题作为R1阻塞，阶段状态见 [PROGRESS](PROGRESS.md)。以下是原实施轮的真实记录。

本轮只修复Windows Qt GUI共用ui::ComboBox，不改网络、上传、协议、SQLite或无关页面布局。保留此前所有未提交改动，没有提交/push。

## 根因与修改

真实控件层级是QComboBox → 顶层Qt::Popup容器 → QListView → viewport。原构造器已把WA_TranslucentBackground、FramelessWindowHint、NoFrame设到正确容器；展开前后运行检查确认这些值保持不变，并非标志被重置。实际弹层抓图四角alpha仍为255；屏幕截图复现内层圆角外有矩形边框/底色和直角阴影。

[Qt6.8.3源码](https://raw.githubusercontent.com/qt/qtbase/v6.8.3/src/widgets/widgets/qcombobox.cpp)中QComboBoxPrivateContainer::paintEvent在菜单风格下独立绘制PE_PanelMenu，随后才是QFrame绘制；因此NoFrame和内层QSS圆角无法阻止该菜单面板。[Qt透明窗口说明](https://doc.qt.io/qt-6/qwidget.html#creating-translucent-windows)也要求顶层透明属性、透明区域绘制和Windows无边框共同成立，仅设置属性不足以覆盖后续不透明绘制。

- `apps/gui/widgets/ComboBox.h`：保留原弹出容器，只过滤它的Paint事件，清空alpha后用QPainter绘制统一10逻辑像素圆角、白色面板、浅边框和轻微圆角阴影；8逻辑像素外边距容纳阴影，关闭平台附加阴影。所有鼠标/键盘/焦点事件仍由Qt原有实现处理。
- 对列表和viewport禁用自动铺底，新增comboPopupList对象名。`apps/gui/styles/Light.qss`仅对这个对象透明化及去内层边框，通用QComboBox样式不改，避免影响Qt对话框等其他控件；浅蓝选中项样式保留。
- showPopup仍调用基类实现，清除菜单风格额外spacer；展开后给阴影留外部空间，使面板宽度不小于ComboBox、左右对齐并限制在当前屏幕可用宽度内。垂直放置和列表滚动仍由Qt负责；没有自造弹出窗口、拷贝Qt私有实现或替换选择逻辑。
- QPainter使用逻辑坐标与抗锯齿，随设备像素比缩放；没有使用整数region硬裁剪圆角。容器/视图仍由QComboBox管理，event filter以组件QObject为生命周期上下文；没有新线程/计时器/第三方UI依赖。

## 自动与视觉验证

新增 `tests/unit/combo_popup_test.cpp`，测试使用真实MainWindow、正式qrc、Fusion浅色palette和隔离QSettings/工作区，不联网。Qt6::Test只链接新增测试目标，不进入产品链接/部署依赖。

修改生产代码前，先运行测试观察真实行为失败：`corner alpha=255 ... popup outer corner must be transparent, not a rectangular fill`。不是根据源文本判断样式是否存在。

| 实际DPR | 普通窗口 | 最大化 | 验证范围 |
| --- | --- | --- | --- |
| 1.0（100%） | 通过 | 通过 | 备份1、日志2、设置2，共5个下拉框 |
| 1.25（125%） | 通过 | 通过 | 同上 |
| 1.5（150%） | 通过 | 通过 | 同上 |

每种状态验证真实popup可见及Qt弹层抓取、四外角透明像素、面板宽度/左缘对齐；鼠标选最后一项、上下方向键、Enter确认、Escape取消、点击外部关闭、激活次数及Enter/Escape后焦点返回。备份页每档普通/最大化保存原生屏幕弹层裁图、带弹层的整窗屏幕截图、独立alpha渲染图。已实际查看截图：无多余矩形底框，阴影随圆角轮廓，浅蓝高亮保持。

当前Windows桌面基础DPR=1.25；通过目标进程QT_SCALE_FACTOR=0.8/1/1.2分别得到实际DPR1/1.25/1.5，日志打印了实际值。没有修改全局Windows显示缩放设置，不能宣称已做系统设置切换或跨屏移动DPI测试。

实际命令（Windows Qt6.8.3 / MinGW13.1 / Ninja / Debug）：

```powershell
$env:PATH='D:/QT/Tools/mingw1310_64/bin;D:/QT/6.8.3/mingw_64/bin;'+$env:PATH
cmake -S . -B build-windows-gui
cmake --build build-windows-gui -j 2
$env:QT_QPA_PLATFORM='windows'
$env:QT_SCALE_FACTOR='0.8' # 实际100%；其他档1 / 1.2
ctest --test-dir build-windows-gui -R '^combo_popup_test$' -V
Remove-Item Env:QT_SCALE_FACTOR
ctest --test-dir build-windows-gui --output-on-failure
```

配置/完整构建退出0，最终完整 **24/24通过**（125.77秒，非性能指标）；新增原生弹层测试28.79秒，既有健康/worker/列表/页面/布局/协议均通过。最终100%专项28.75秒、150%专项28.76秒；125%包含在最终全量及之前专项中。日志在忽略目录 `build-windows-gui/combo-artifacts/{scale-100.log,scale-125.log,scale-150.log,full-ctest.log}`，全量LastTest.log包含实际DPR信息。

截图在同一combo-artifacts目录：`reported-before.png`是用户提供原图的本地副本；`after-1-normal-screen.png`、`after-1.25-normal-screen.png`、`after-1.5-maximized-screen.png`等为原生屏幕截图；`after-1.25-normal-alpha.png`为独立组件渲染。整窗`*-window.png`包含当前原生MainWindow及popup。均为忽略的本地验证产物，不上传Git。部分屏幕截图含桌面软件的弹窗拦截提示，非ChunkHarbor绘制；未改变系统设置，独立alpha图另行检查实际面板。

独立只读审查未发现生产阻断问题，审查者未重跑测试。按审查意见将测试标记`native-desktop`、RUN_SERIAL，避免误当作offscreen测试；无桌面验证时可用`ctest -LE native-desktop`仅执行其他测试，但须明确弹层视觉未验证。

## 失败记录与验证边界

初始测试编译误用findChild<ui::ComboBox*>（该薄封装无Q_OBJECT），改为查找QComboBox后dynamic_cast，编译成功才取得上述行为红测。最初QTest QWidget直送鼠标事件未经过完整窗口抓取路径，选择被忽略；改用QTest QWindow事件并等待Qt开菜单防误触释放期后通过，没有修改生产输入处理。尝试系统级SetCursorPos返回false，未修改输入权限；本轮是原生Windows显示+QTest自动事件验证，不是人工物理鼠标验收。

已知既有非阻断告警：Vulkan开发头缺失、windeployqt缺dxcompiler/dxil；不代表正式安装包验证。Linux GUI、MSVC、Release/Sanitizer、不同Qt版本、系统缩放设置切换、混合DPI跨屏移动、屏幕边缘所有组合及人工输入未验证。共用组件在当前5个实际使用点均回归；未来更换Qt风格/版本或加入长菜单应复核弹层paint路径与滚动。
