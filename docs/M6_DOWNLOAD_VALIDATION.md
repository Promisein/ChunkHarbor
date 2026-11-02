# M6 Linux CLI 下载验收（2026-11-02）

本轮完成服务端与Linux CLI单文件恢复；GUI恢复、下载续传/覆盖及历史版本选择不在本轮范围。协议与线程/资源说明见[M6_DOWNLOAD.md](M6_DOWNLOAD.md)。

## 用法

用包含本轮源码的Ubuntu构建产物启动upload服务模式；既有旧二进制不会自动获得下载能力。以下输出父目录必须存在，目标文件必须不存在：

```bash
./build/chunkharbor-cli download --path examples/demo.txt --output ./restored-demo.txt
```

可通过`--port 18787`连接SSH转发端口；`--host`仅接受127.0.0.1。可传`--version-id`作为“当前版本仍然等于该ID”的前置条件，不提供历史版本选择。成功输出`DOWNLOAD OK: size=... sha256=...`，失败退出2。输出路径由用户明确指定，服务端逻辑路径不参与本地路径拼接。

## 实际环境与命令

Windows Qt6.8.3 / MinGW13.1 / Ninja，既有Debug构建：

```powershell
$env:PATH='D:/QT/Tools/mingw1310_64/bin;D:/QT/6.8.3/mingw_64/bin;'+$env:PATH
cmake --build build-windows-gui -j 2
ctest --test-dir build-windows-gui --output-on-failure
```

构建成功，完整 **20/20通过**，含新增可移植下载协议测试与既有GUI回归。GUI下载并未实现或测试。windeployqt仍报告缺少dxcompiler/dxil的非阻断警告，不作为正式打包通过证据。

Ubuntu22.04 / GCC11.4，通过SSH别名miniob-ubuntu在新建隔离目录执行：

```bash
cmake -S /tmp/chunkharbor-m6-download-zjetPmKD \
  -B /tmp/chunkharbor-m6-download-zjetPmKD/build \
  -DCMAKE_BUILD_TYPE=Debug -DCHUNKHARBOR_BUILD_GUI=OFF \
  -DSQLite3_INCLUDE_DIR=/home/ning/ChunkHarbor/build/deps/sqlite/usr/include \
  -DSQLite3_LIBRARY=/usr/lib/x86_64-linux-gnu/libsqlite3.so.0 \
  -DOPENSSL_INCLUDE_DIR=/home/ning/ChunkHarbor/build/deps/openssl/usr/include \
  -DOPENSSL_CRYPTO_LIBRARY=/usr/lib/x86_64-linux-gnu/libcrypto.so.3 \
  -DCMAKE_CXX_FLAGS=-I/home/ning/ChunkHarbor/build/deps/openssl/usr/include/x86_64-linux-gnu
cmake --build /tmp/chunkharbor-m6-download-zjetPmKD/build -j 2
ctest --test-dir /tmp/chunkharbor-m6-download-zjetPmKD/build --output-on-failure
```

配置/构建成功，最终完整 **50/50通过**，29.08秒，不作为性能指标。configure.log、build.log、ctest.log位于该隔离根。SQLite/OpenSSL开发头只读复用既有build/deps，未修改主工程或真实备份。测试创建专用TemporaryDirectory根，退出清理其测试数据。

下载专项：

```bash
ctest --test-dir /tmp/chunkharbor-m6-download-zjetPmKD/build -R download --output-on-failure
```

## 覆盖及真实结果

| 测试 | 验证内容 |
| --- | --- |
| download_test | 严格JSON字段/路径/版本/整数/范围、64MiB/256KiB边界、二进制块大端布局与截断、32/33帧类型 |
| download_integration | 真实CLI上传→下载→内容一致；空文件、中文、多块二进制；旧upload即时下载与新delta版本；新版本发布后原会话仍读旧版本；重启后恢复 |
| 服务端拒绝 | 未提交文件、缺能力、错误版本/路径/范围、缺会话、重复BEGIN；对象损坏拒绝、服务端对象符号链接/FIFO拒绝且不阻塞 |
| 本地安全 | 已有目标/符号链接/父目录链接不覆盖，远端逻辑路径穿越不形成目标，正常失败无临时文件遗留 |
| download_peer_test | 17类：成功、错误元信息/请求ID/响应类型/块版本/偏移/长度/块哈希/整文件哈希、截断、部分数据已写入后断开、最终文件/链接抢占、父目录替换后仍限制于固定目录FD |
| download_fault_test | LD_PRELOAD四类：ENOSPC与文件fsync失败不发布且清理；目录fsync失败返回失败但保留已发布且校验正确的文件；短写/EINTR重试后成功 |

独立只读静态审查核对协议、工作池和连接生命周期、版本固定与本地安全发布，未发现Critical/Important问题；审查者未独立重跑测试。

## 失败记录和边界

- 新测试先对上一轮服务端运行，按预期因缺少download_v1断言失败。
- Ubuntu首次构建误把SqliteStore包装器传给sqlite3语句构造，改为nativeHandle后重建成功。
- 一次测试夹具使用超限控制帧，CLI正确提前拒绝，测试端发送产生BrokenPipe；改成长度合法的错误数据帧，之后完整回归通过。
- 未执行Release、ASan/UBSan/TSan、MSVC、Linux GUI、新的Windows GUI真实上传端到端、GUI恢复、真实磁盘满或硬件掉电，也未验证64MiB最大文件端到端或性能/大规模并发指标。
- 不覆盖现有文件；路径要求父目录已存在且没有符号链接，拒绝..。用户控制目标目录，不对抗恶意同UID/root。
- 正常异常清理临时文件；强制终止可能留下`.chunkharbor-download-*`，没有自动续传或自动清理未知文件。
- rename成功但父目录fsync失败时，文件可能存在，CLI明确报失败，不能把“失败”理解为“目标一定不存在”。
- 服务端首次打开全文件校验增加一次读盘成本；后续拉取固定FD，未做批量读取或sendfile优化。当前仍限可信loopback/SSH，无TLS/认证。

本轮未提交或推送，Ubuntu主工程二进制未被本轮测试替换。
