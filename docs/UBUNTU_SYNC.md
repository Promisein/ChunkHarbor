# Ubuntu主工程同步（2026-11-02）

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
