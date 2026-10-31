# Linux 安全匿名暂存文件（M2 子任务）

`storage/TempFileStore.h` / `src/storage/TempFileStore.cpp` 位于Linux专用 `chunkharbor_server_core`。共享protocol/client_core不包含Linux源码。该组件自身只创建匿名文件；M2已由UploadStorage使用它，并安全链接为具名会话数据后接入DiskExecutor、发布与持久块回执，详见M2_USAGE.md。

## 接口及设计

`TempFileStore(absoluteRoot)`打开一个已经存在的服务器配置目录，要求绝对路径、所有路径分量无符号链接、目录属于服务进程有效UID，并且group/other权限位全部为0（通常0700）。不会自动创建目录、chmod配置根目录或清理其中已有文件。路径拒绝NUL/控制字节、反斜杠、冒号、点/点点及空分量，总长最多4096字节、分量最多255字节。

根目录FD通过RAII固定对象身份。之后配置路径被重命名/替换，不会重定向已有store；需要切换根目录时必须显式新建组件。受信任服务账号须保持根目录权限和命名空间受控；本组件不对抗本地root、恶意同UID进程或管理员将已打开的目录移出根目录。这些主体能改变权限或直接访问服务进程FD，不属于远端协议输入威胁模型。

`create(relativeDirectory=".") const`在根内已有目录的文件系统上创建一个新的、空的、无名字的普通文件，返回拥有该FD的 `UniqueFd`。参数是内部Linux目录选择，不是客户端文件目标名；`.`仅用于选择根本身。非默认目录必须为相对路径，按上述字节/分量规则拒绝绝对路径、`..`、重复/尾随斜杠等。此接口接受Linux原生路径字节，不负责UTF-8转换；协议UTF-8逻辑路径验证由BEGIN_UPLOAD完成，不能直接用客户端路径作为目录选择。当前不创建嵌套目录。

创建使用一次 `openat2(rootfd, directory, O_TMPFILE|O_RDWR|O_CLOEXEC, 0600)`，设置 `RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_MAGICLINKS|RESOLVE_NO_XDEV`：内核在解析过程中限制根目录、拒绝各级链接和跨挂载点，避免用户态“检查后再次打开”的窗口。EINTR重试；EAGAIN及其他错误直接返回调用方，避免无界重试。不支持时不会退回普通路径拼接或命名临时文件。

匿名文件没有客户端可选择/猜测的目录项，因此没有按路径unlink清理时删除错误文件的竞态，也不会打开或截断已有目标文件。创建后fchmod保证0600（即使继承了限制性umask），fstat确认普通文件且链接数为0；中途失败由局部UniqueFd自动关闭。返回文件FD带CLOEXEC；store及文件均可移动、不可复制，文件可以独立于store存活。并发create只读固定根FD，但调用方不得并发移动/销毁store。创建和文件I/O可能阻塞，后续须放DiskExecutor工作线程，不能在EventLoop回调里执行。

实现参考Linux项目的 [openat2手册](https://www.man7.org/linux/man-pages/man2/openat2.2.html) 与 [O_TMPFILE说明](https://man7.org/linux/man-pages/man2/open.2.html)。要求Linux5.6+、openat2构建头及支持O_TMPFILE的文件系统；当前VM内核6.8、/tmp为ext文件系统，已实际通过。Windows不构建此组件。跨挂载点限制未通过真实bind mount测试（需额外权限），不宣称已覆盖所有文件系统。

## 持久性和后续发布边界

匿名暂存是同进程内的原语：未链接前关闭最后一个FD或进程退出就丢弃数据。TempFileStore自身不执行fsync或ACK；UploadStorage在ACK前已将其链接并持久化为.sessions数据，但仍未提供M3重启续传。

后续发布需要单独设计：安全关联目标目录与逻辑路径、整文件校验、持久化，将匿名inode安全链接为受控的具名暂存文件，再按约定进行无覆盖/版本检查和rename、目录fsync及元数据协调。本轮未设置O_EXCL以保留后续linkat可能性，但未实现或验证实际链接/发布；普通账号的linkat权限、/proc依赖及跨文件系统失败需另行测试。M3需另加具名暂存/持久会话注册与恢复策略，不能把本组件当成已完成的断点续传。

远端用户不得得到原始FD或控制保留的暂存命名空间。调用方还需控制同时打开文件数、磁盘配额、文件大小、块写入范围及会话生命周期；O_TMPFILE本身不提供这些资源上限。本轮不更改服务端监听与现有Reactor。

## 测试和调试

测试只使用独占 `/tmp/chunkharbor-storage-XXXXXX` 树，根/外部哨兵均在该测试树内；退出只清理该树，不使用真实用户数据。

- 创建空文件、0600/CLOEXEC/链接数0、二进制测试读写、限制性umask。
- 根/中间/最终目录符号链接、内部链接也拒绝；非法目录及已有普通文件拒绝。
- 目录与外部符号链接反复替换时，只允许安全匿名FD或错误；外部哨兵和目录项不变。这是并发回归，不是所有调度的形式化证明。
- 根路径替换后的FD身份、移动语义、文件独立于store存活、反复成功/失败后的FD计数。
- 链接包装注入ENOSYS、EOPNOTSUPP、EMFILE、EAGAIN失败及EINTR重试；fchmod/fstat失败验证打开后的FD也释放，无弱化fallback。

Ubuntu沿用正式目录缓存（OpenSSL依赖见SHA256.md）：

```bash
ssh miniob-ubuntu
cd /home/ning/ChunkHarbor
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 2
ctest --test-dir build --output-on-failure
ctest --test-dir build -R '^temp_file_store_test$' -V
gdb --args ./build/chunkharbor-temp-file-store-test
# break chunkharbor::TempFileStore::create
# run
```

ASan/UBSan使用build-asan并启用 `-DCHUNKHARBOR_SANITIZERS=ON`，构建后运行：

```bash
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-asan --output-on-failure
```

未进行真实磁盘满/配额测试、bind mount、断电、kill -9恢复、发布或吞吐量测试。内存随路径长度O(N)（输入有界），每次创建使用O(1)个FD；组件不会读取文件内容或按声明文件大小分配内存。
