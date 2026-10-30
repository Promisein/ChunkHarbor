# M1 Buffer 设计与补充审查

此文记录前一轮独立 Buffer 的设计和当时的审查。随后完成 M1 时已实现 Channel owner 绑定与生命周期测试、协议 target 拆分及 TCP 网络组件；最新状态和验证见 `ARCHITECTURE.md`、`M1_VALIDATION.md`。下文的“尚未修复/建议”保留为历史设计依据，不代表当前仍缺少相应实现。

## Buffer 的所有权与不变量

`std::vector<char>` 独占连续内存，通过 RAII 释放；禁止复制和移动，避免连接缓冲被意外复制或移后索引失配。支持任意二进制字节，长度不依赖 NUL。单个 Buffer 由其连接所属 EventLoop 串行访问，不内置互斥锁。

不变量：`0 <= readIndex <= writeIndex <= storage.size() <= maxCapacity`。可读区间是 `[readIndex, writeIndex)`；尾部可写区间是 `[writeIndex, storage.size())`；已消费前缀可以回收。`capacity()` 表示可用存储长度，不是 `vector::capacity()`。

默认初始容量 1024 字节、最大容量 4 MiB，可在构造时设置；允许零容量。初始值大于上限报 `invalid_argument`。申请空间前用 `length > maxCapacity - readableBytes` 检查，避免先相加引起溢出；超限报 `length_error`。读、消费、确认写入越界报 `out_of_range`，不静默截断。分配失败保留原数据并传播异常。

尾部空间足够则直接写；否则若回收前缀即可容纳，使用支持重叠区域的 `memmove`；仍不足时，分配新数组，目标为“现容量翻倍”和“可读字节 + 需求”的较大者，翻倍受最大容量约束，只复制未读数据。完全消费后两索引归零，容量保留供复用，不在每次消费时缩容。

最大容量约束单个存储区的逻辑长度，不能当作进程内存承诺：扩容期间旧区与新区同时存在，自身视图追加还会短暂生成快照；另有 allocator 开销。大量连接的总内存、写缓冲高低水位和背压仍需 TcpConnection 层控制。

## 与后续模块的接口

- FrameCodec 接收 `readableView()`，先判断是否有完整且合法的帧；成功处理后调用 `consume(n)`。不完整帧保留在缓冲内等待下次追加；视图不拥有内存，不能跨 Buffer 修改或跨异步回调保留。需要异步处理的数据必须显式取得所有权。
- TcpConnection 可 `ensureWritable(n)`，然后向 `writableData()` 接收不超过 `writableBytes()` 的字节，只有 recv 返回正数才 `commitWrite(actual)`；EAGAIN、EINTR 和错误不确认长度。输出方向可直接发送可读区间，只消费实际发送字节。Buffer 自身不依赖 socket，也不调用 recv/send。
- `append()` 一般只复制一次输入；输入指向自身存储时先保留快照，防止整理或扩容使源指针失效。`read(n)` 返回独立字符串并消费，会产生复制；优先使用视图加消费处理同步解析。
- 帧 payload 上限、32 字节帧头和流缓冲容量是不同概念；后续 Codec 必须在分配前检验帧长度。每次网络读取也必须受剩余容量限制，不可无限累积粘包。

## 时间复杂度与 C++ 知识

容量、可读/可写字节查询、视图和 consume/commitWrite 为 O(1)。普通追加 n 字节为 O(n)，整理额外 O(未读字节)，扩容还包含新数组初始化与分配成本。read(n) 为 O(n)。几何增长减少反复分配，但“满缓冲反复消费一字节再追加一字节”可能反复整理，不能宣称所有负载都摊还 O(1)。这是连续缓冲的取舍，只有测量证明整理成为瓶颈后再考虑环形/分段结构。

本组件涉及 vector RAII、连续存储、string_view 非拥有视图、size_t 溢出检查、memmove 与 memcpy 的区别、异常安全，以及按线程归属替代内部加锁。设计为 TCP 字节流保留跨读取的未完成数据和连续解析视图，有利于减少分配与复制；本轮没有吞吐量或并发性能测量。

## Channel 回调生命周期审查

1. `Channel::handleEvent()` 连续访问 error/read/write 回调。任一回调若销毁 Channel 或所属连接，后续访问可能悬空；甚至正在执行的 std::function 也可能随对象销毁。当前代码注释要求外部保活，但没有强制机制。epoll token 只能避免已移除注册的后续事件，无法保护正在执行的回调。
2. 回调里 remove/disableAll 不会改变 handleEvent 已收到的事件掩码，仍可能执行同一次派发的其他回调。关闭后的读写回调应有明确策略并测试。
3. 建议在引入 TcpConnection 前增加可选 weak owner 绑定，在整个派发期间持有强引用；关闭先取消注册，连接与 Channel 的最终释放延迟到派发完成。注册前无 owner 的生命周期也必须清楚定义。需要覆盖回调关闭、自释放请求、多事件及过期注册等真实回归测试。
4. 当前析构中的 remove 可能抛出，而析构默认 noexcept；线程检查还依赖 assert。后续应定义所属线程销毁和不抛异常的清理策略。本轮未改相关行为。

## 跨平台协议库拆分建议

增量创建 `chunkharbor_protocol`，仅包含固定宽度帧字段、字节序、长度校验、增量解析和 golden vectors；不包含 Qt、POSIX、Channel 或 Buffer 头。Codec 可接收纯 C++17 string_view，并返回消费长度/等待更多数据/错误，由传输适配器管理缓冲。

Linux 的 Reactor 后续归入 `chunkharbor_linux_net`；平台相关 UniqueFd 不进入可移植公共库。当前 Buffer 本身没有 OS 依赖，可留作 Linux 网络组件，协议不反向依赖它；若将来 Qt 适配确有复用需求，再单独迁入可移植基础库。client_core 待实际客户端状态机出现时建立，避免空 target。CMake 选项和平台门控在独立小任务中补齐，本轮保留既有 target 结构。

## 验证

Ubuntu 正式目录 `/home/ning/ChunkHarbor`：CMake 配置、构建通过，CTest 5/5 通过。Buffer 测试包含空缓冲、分段输入、部分消费、空间整理、扩容、零/最大容量、超限不破坏内容、整数极限、自身数据追加、直接写入、嵌入 NUL，以及与 deque 对照的 5000 次确定性操作。实现前构建因缺少 Buffer.h 失败，随后实现通过。

额外运行并通过：

```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic -g -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Iinclude src/net/Buffer.cpp tests/unit/buffer_test.cpp \
  -o build/buffer-sanitized
./build/buffer-sanitized
```

未验证：Windows 编译、TCP 集成、Channel 自释放故障测试、性能与多客户端测试。
