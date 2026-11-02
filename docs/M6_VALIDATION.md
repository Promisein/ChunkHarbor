# M6 分页文件列表验收（2026-11-02）

本轮仅交付受限分页协议与服务端查询。CLI和GUI尚无文件列表命令/页面接入，下载恢复未实现。契约见[M6_FILES_LIST.md](M6_FILES_LIST.md)。

## 实际平台和命令

Windows Qt6.8.3 / MinGW13.1 / Ninja，既有build-windows-gui Debug配置：

```powershell
$env:PATH='D:/QT/Tools/mingw1310_64/bin;D:/QT/6.8.3/mingw_64/bin;'+$env:PATH
cmake --build build-windows-gui -j 2
ctest --test-dir build-windows-gui --output-on-failure
```

最终构建成功，完整CTest **19/19通过**，包含新增files_list_test与既有GUI回归。windeployqt报告找不到dxcompiler/dxil的警告，构建及测试退出0；不据此宣称正式安装包验收。

Ubuntu 22.04虚拟机 / GCC11.4 / 内核6.8.0-138-generic，通过SSH别名miniob-ubuntu执行。源码归档解压到新建`/tmp/chunkharbor-m6-list-KhFt0dZP`，依赖只读复用既有主工程build/deps；未修改主工程源码、服务或用户备份根。

```bash
cmake -S /tmp/chunkharbor-m6-list-KhFt0dZP \
  -B /tmp/chunkharbor-m6-list-KhFt0dZP/build \
  -DCMAKE_BUILD_TYPE=Debug -DCHUNKHARBOR_BUILD_GUI=OFF \
  -DSQLite3_INCLUDE_DIR=/home/ning/ChunkHarbor/build/deps/sqlite/usr/include \
  -DSQLite3_LIBRARY=/usr/lib/x86_64-linux-gnu/libsqlite3.so.0 \
  -DOPENSSL_INCLUDE_DIR=/home/ning/ChunkHarbor/build/deps/openssl/usr/include \
  -DOPENSSL_CRYPTO_LIBRARY=/usr/lib/x86_64-linux-gnu/libcrypto.so.3 \
  -DCMAKE_CXX_FLAGS=-I/home/ning/ChunkHarbor/build/deps/openssl/usr/include/x86_64-linux-gnu
cmake --build /tmp/chunkharbor-m6-list-KhFt0dZP/build -j 2
ctest --test-dir /tmp/chunkharbor-m6-list-KhFt0dZP/build --output-on-failure
```

最终配置/构建成功，完整CTest **46/46通过**。build.log及ctest.log保留于隔离源码根，最后全量用时29.40秒，不作为性能基准。

专项可单独复现：

```bash
ctest --test-dir /tmp/chunkharbor-m6-list-KhFt0dZP/build -R files_list --output-on-failure
```

## 验证覆盖

- 严格协议编解码：合法/空结果、limit上下界、重复键/额外字段、非法路径/UTF-8、整数拒绝浮点、超过2^53的大小无损往返、版本标识、重复路径与游标一致性。
- 真实TCP：能力独立协商、缺少HELLO/能力拒绝、非法请求返回稳定错误码；沿用原request_id/帧头。
- 存储：空库、旧upload当前进程提交即时可见；列表不触发legacy_versions迁移；新旧记录合并、同路径仅当前版本、未完成与preview不展示；重启后内容不变。
- 分页：中文路径按UTF-8字节排序，单条分页无漏项/重复；66个额外空文件验证64条上限；20个含大量双引号的深路径验证JSON转义后仍不超过65536字节并提前截页，完整遍历与预期集合一致。
- 会话：同一连接中插入列表查询后，原增量会话仍可继续并提交，查询不改变绑定。
- 查询计划：用本机Python SQLite加载现有v1/v2 schema，对源码中的查询执行EXPLAIN QUERY PLAN，显示MERGE(UNION ALL)、current_files路径索引SEARCH、versions主键SEARCH、committed_transfers_by_path范围SEARCH，无全量排序。

## 失败记录与限制

旧服务端先运行新TCP测试，按预期因能力缺失断言失败。实现首轮编译缺DeltaJson所需类型定义，补齐独立头依赖后重新构建与全量测试通过；编译失败后的Not Run不计通过。

未运行Release、Sanitizer、MSVC、Linux GUI、新的Windows GUI真实上传端到端、下载恢复或真实性能实验。未承诺跨页快照、总文件数或用户权限隔离；当前仅限可信loopback/SSH。索引创建是schema v2上的幂等兼容增强，查询本身只读，不重验文件正文。旧upload返回的version_id沿用session_id，后续下载需兼容既有legacy版本物化流程。
