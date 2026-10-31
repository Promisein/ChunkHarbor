# 有界 DiskExecutor（M2 独立组件）

`DiskExecutor(loop, workers, capacity)` 绑定已有EventLoop，固定1–64个工作线程，容量1–65536。trySubmit可从多个线程调用；返回Accepted/Full/Stopped，队列满不等待、不覆盖任务。空Work/Completion抛invalid_argument。

容量覆盖所有已接受但回调尚未返回的任务：等待队列 + 运行任务 + 已完成结果 + 正在执行的完成回调。因此消费者不处理结果时，生产者也不能继续无限堆积。std::any返回值与闭包捕获本身的大小不受数量限额约束；后续文件任务应只传有界块或元数据，不能捕获整个大文件。

Work返回std::any；所有工作异常被exception_ptr捕获。Result在loop线程交给Completion，调用方用any_cast读取已约定的类型，或rethrow_exception处理失败。工作线程只操作互斥锁保护的队列和eventfd，不操作Channel、连接或loop状态。完成结果通过独立eventfd/Channel投递，因此不会因EventLoop普通pending队列满而丢失。

构造、stop、析构归属loop线程，且执行器必须先于loop销毁。stop先停止接收、排空工作并join，再在loop线程派发剩余结果，可在loop.run返回后调用；幂等。它可能等待磁盘操作，不能在正常网络回调内作为日常等待手段。任务必须有限，不能等待loop完成回调，否则stop可能死锁；不可中断的磁盘I/O也无法保证关闭时限。

析构是兜底取消路径：停止接收，完成已接受的Work并join，丢弃尚未派发的Completion，不调用应用回调。要求可靠收尾时必须显式stop。不能从工作或完成回调内stop/销毁执行器；stop检测回调重入并抛logic_error，回调内析构是契约违反并terminate。

派发每次只取进入时已有的结果批次，避免不断提交使单次派发无限延长。完成回调异常会被捕获，继续派发本批其他结果并释放容量，然后重抛首个异常；应用应在回调中自行处理业务错误，不能把异常当正常控制流。回调应轻量且不能销毁执行器。

未来回调访问连接时应捕获weak_ptr，在loop线程lock并检查连接状态；不能捕获悬空栈引用或把磁盘线程直接绑定到socket。当前组件未接入上传/握手，不执行真实文件、SHA-256或SQLite业务。

新增文件：concurrency/DiskExecutor.h、src/concurrency/DiskExecutor.cpp、disk_executor_test.cpp；暂链接Linux网络库，协议和Windows共享target不依赖它。未新增全局线程池或重构Reactor。

测试覆盖工作/完成线程归属、结果与异常传回、满/停拒绝、停止排空与重复stop、析构取消、回调异常后继续派发、普通loop队列满、多个提交线程。验证命令：

```bash
cd /home/ning/ChunkHarbor
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 2
ctest --test-dir build --output-on-failure
ctest --test-dir build -R disk_executor -V
```

未做真实磁盘故障、性能、TSan或Windows执行器测试。当前不保证任务执行/完成顺序，多个工作线程可乱序完成；后续会话层须按业务约束串行化写入和提交。

独立静态审查未发现阻断问题。内存耗尽不是本组件保证恢复的业务失败：完成队列内部容器分配失败可能导致工作线程异常逃逸并terminate；数量上限不能替代字节配额或OOM策略。
