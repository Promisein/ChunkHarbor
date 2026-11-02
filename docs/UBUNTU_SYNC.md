# Ubuntu主工程同步（2026-11-02）

## 最新：M6同步到原工程（2026-11-02）

按用户要求，后续默认在`/home/ning/ChunkHarbor`增量构建与使用，不再为每轮验证创建新的源码工程目录。原`build`和依赖缓存继续复用；备份数据应单独存放，不能把源码目录当服务端存储根。

同步前Windows和Ubuntu HEAD均为`bec627014ef2e7a73f1115aba6a9da597e6100eb`，Ubuntu main工作区干净，未发现运行中的ChunkHarbor服务。Windows最新M6仍未提交，因此此次通过源码归档同步工作区内容，不执行pull、reset、commit或push，Git历史和原stash保持不变。

同步242个Git跟踪/未忽略源码文件，包含GUI代码和M6协议、分页、Linux下载及测试；排除`tests/testFiles`的照片/ZIP、构建产物、SDK、Git内部及备份数据。传输文件放原`build`，逐项核对SHA-256，审计清单保存在原仓库`.git/codex-m6-sync-manifest.json`。未覆盖用户测试数据或备份根，未新增源码工程文件夹。

在原工程实际执行下文相同配置（GUI=OFF、SERVER/CLI/TESTS/HASH=ON及既有SQLite/OpenSSL路径），`cmake --build build -j 2`成功，`ctest --test-dir build --output-on-failure`完整 **50/50通过**。日志：`build/m6-configure.log`、`build/m6-build.log`、`build/m6-ctest.log`。本轮未重跑Windows GUI/MSVC/Release/Sanitizer，不将历史结果当本轮重跑。

现在日常入口统一为：

```bash
cd /home/ning/ChunkHarbor
./build/chunkharbor-server --help
./build/chunkharbor-cli --help
```

Linux CLI的help已包含download；服务端当前支持file_list_v1/download_v1。M6工作区改动在两端都未提交，属预期状态。原工程已完整构建，不再依赖之前/tmp工程的源码或schema文件。

以下为此前M5同步的历史记录。

主工程路径：`/home/ning/ChunkHarbor`，分支main，origin仍为原GitHub地址。已从早期M1工作树快进至完整M5代码，包含GUI源码但GUI目标关闭，不安装Qt，不重建其他测试工程目录。

## 旧改动处理

Git状态最初按目录合并显示74项，展开后实际涉及82个文件。逐文件与M1/M2/M3/M4/M5对象比较：79个文件与已有历史版本一致；剩下只有README本机路径、旧PROGRESS文档差异及EchoHandler.cpp尾部空格，没有独有功能代码需要移植。

全部82个文件（含未跟踪项）已通过`git stash push --include-untracked`归档，stash名称为`archive Ubuntu M2 working tree before M5 synchronization 2026-11-02`。对stash工作树对象和第三父对象中的文件逐个验证SHA256，确认完整保存后才快进更新；没有直接reset或覆盖旧改动。审计清单在主项目`.git/codex-ubuntu-sync-audit.json`，不上传GitHub。

查看归档（Ubuntu执行，仅查看，不应用旧代码）：

```bash
cd /home/ning/ChunkHarbor
git stash list
git stash show --stat --include-untracked 'stash@{0}'
```

不要直接把整份旧M2 stash应用到最新M5；如需恢复某个历史文件，应单独检查并提取。Git stash位于主项目内部，不在/home/ning创建额外备份文件夹。

## 传输与验证

Ubuntu直连GitHub曾失败，因此Windows通过已有系统代理获取origin/main，再用Git bundle经SSH导入Ubuntu对象与origin/main，执行`git merge --ff-only origin/main`。提交对象与GitHub一致，不是只覆盖源码的无版本复制。临时bundle在导入后移除，远端URL保持不变。

当前依赖：SQLite头位于`build/deps/sqlite/usr/include`，OpenSSL头位于`build/deps/openssl/usr/include`，链接既有系统库；依赖与build均忽略，不进入Git。

实际执行：

```bash
cd /home/ning/ChunkHarbor
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DCHUNKHARBOR_BUILD_GUI=OFF -DCHUNKHARBOR_BUILD_SERVER=ON \
  -DCHUNKHARBOR_BUILD_CLI=ON -DCHUNKHARBOR_BUILD_TESTS=ON \
  -DCHUNKHARBOR_BUILD_HASH=ON \
  -DSQLite3_INCLUDE_DIR=/home/ning/ChunkHarbor/build/deps/sqlite/usr/include \
  -DSQLite3_LIBRARY=/usr/lib/x86_64-linux-gnu/libsqlite3.so.0 \
  -DOPENSSL_INCLUDE_DIR=/home/ning/ChunkHarbor/build/deps/openssl/usr/include \
  -DOPENSSL_CRYPTO_LIBRARY=/usr/lib/x86_64-linux-gnu/libcrypto.so.3 \
  -DCMAKE_CXX_FLAGS=-I/home/ning/ChunkHarbor/build/deps/openssl/usr/include/x86_64-linux-gnu
cmake --build build -j 2
ctest --test-dir build --output-on-failure
```

配置、构建成功，Debug44/44通过。GUI源码存在且GUI=OFF，主工程工作区干净。Windows接收SSH中文输出时曾因默认GBK解码报错；远端归档与同步本身成功，随后独立核查HEAD、stash及干净状态确认，不把终端解码错误当作未发生的Git操作。

本轮未重跑Windows GUI、Linux GUI或Release/Sanitizer；此前测试结果仍为各自历史记录。后续可直接使用此主工程的`./build/chunkharbor-server`启动服务端。
