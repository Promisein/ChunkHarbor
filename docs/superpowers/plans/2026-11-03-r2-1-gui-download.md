# R2.1 Windows GUI 当前单文件下载

用户已明确授权本轮实现及验证；本计划在当前工作区直接执行，不提交、不推送，保留已有 PROGRESS 修改。

## 设计与接口

- 复用 download_v1、HandshakeExchange、Download 编解码；不改变 protocol/server/schema。
- DownloadController(QObject，GUI线程) 接收固定的 host/port/ListedFile/本地目标，最多一个任务，提供 start/cancel、busy、状态及进度；工作线程独占 QTcpSocket 和输出句柄，queued 回调带 QObject context。
- 每次拉取至多256KiB，写入并校验后再请求下一块。网络响应有界、请求超时，等待周期内检查取消。
- DownloadOutput 为不可复制的 RAII 输出对象。Windows 逐组件打开并保留不共享 DELETE 的目录句柄、拒绝重解析点，CREATE_NEW 建立同目录独占临时文件；FlushFileBuffers 后由句柄以 ReplaceIfExists=FALSE 重命名，异常用句柄删除临时文件。仅允许本地盘绝对路径，拒绝设备/UNC/ADS/保留名。
- 取消与发布用原子状态CAS确定提交点：提交点前取消不发布；进入发布后等待发布结果，取消不再反转结果。GUI不等磁盘锁。关窗先异步取消，任务结束后关闭；销毁控制器取消并join，再销毁QObject。
- FilesPage 从代理索引映射原始 FileEntry，保存逻辑路径/版本/大小/摘要和端点；原生保存框只给建议文件名，用户路径不由服务端拼接。显示文件名/字节/百分比/状态，忙时拒绝重复任务。

## 执行步骤

1. 先添加下载探针及TCP对端测试，验证基线缺少实际下载入口（RED）。
2. 实现安全输出、独立控制器和后台下载；接入文件页、MainWindow关闭流程及CMake。
3. 运行Windows配置/构建，执行空/普通/多块/中文、完整性、断连/超时/取消、已有及竞争目标、版本不符、重复启动/关窗测试；修正后跑现有CTest。
4. 在专用Ubuntu临时根启动既有server，经SSH转发执行真实GUI下载，逐项比较SHA-256；运行R1上传模式回归。
5. 更新PROGRESS和R2.1验证文档，记录命令/结果/限制，检查diff；不自动commit/push。

## Review focus

攻击性对端测试验证错request/type/path/version/offset/length/块及整文件摘要、超限/浮点大小、半包和截断；文件测试验证目标竞争、目录替换和临时清理。取消靠同步状态而非网络事件触发。旧端上传和下载协议保持原语义；单文件64MiB、无下载续传、无历史/目录下载。
