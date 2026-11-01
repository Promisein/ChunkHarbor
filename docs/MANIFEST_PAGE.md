# M4 MANIFEST_PAGE

本轮仅实现type18正文编解码及可信文件上下文校验，没有接入服务端增量状态机、基线存储或CLI增量上传；HELLO不新增能力。

接口位于`include/chunkharbor/protocol/ManifestPage.h`。四字段为session_id、page_index、first_block_index、hashes；每页1–512个32字节摘要，线上使用小写hex，起始块必须等于页号乘512。最多128页、65536块；控制正文最多65536字节。

编解码检查单页结构；`validateManifestPageForFile`另检查可信会话ID、期待页号、文件大小和块大小。非末页必须512项，空文件不允许页面；不能用空页当作清单完成。详细格式见首个设计文档。

返回对象拥有ID和摘要，输入string_view不跨调用保存。模块不计算哈希、不操作磁盘/网络、不持有跨页状态；重复页重试等业务语义由未来状态机负责。

## 本轮实际验证

Windows Qt6.8.3 / MinGW13.1：沿用`build-windows-m3`缓存，构建自动重新生成CMake；新增测试在空实现上先以`manifest assertion failed`失败，再接入实现。

```powershell
$env:PATH='D:\QT\Tools\mingw1310_64\bin;D:\QT\6.8.3\mingw_64\bin;'+$env:PATH
cmake --build build-windows-m3 -j 2
ctest --test-dir build-windows-m3 --output-on-failure
```

最终完整CTest **10/10通过**。

Ubuntu：源码单独复制到新建隔离目录`/tmp/chunkharbor-m4-aaEMsbwA`，未修改既有工程/备份。开发头沿用已有私有依赖缓存：

```bash
cd /tmp/chunkharbor-m4-aaEMsbwA
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DSQLite3_INCLUDE_DIR=/home/ning/ChunkHarbor-m3-verify/deps/sqlite/usr/include \
  -DSQLite3_LIBRARY=/usr/lib/x86_64-linux-gnu/libsqlite3.so.0 \
  -DOPENSSL_INCLUDE_DIR=/home/ning/ChunkHarbor/build/deps/openssl/usr/include \
  -DOPENSSL_CRYPTO_LIBRARY=/usr/lib/x86_64-linux-gnu/libcrypto.so.3 \
  -DCMAKE_CXX_FLAGS=-I/home/ning/ChunkHarbor/build/deps/openssl/usr/include/x86_64-linux-gnu
cmake --build build -j 2
ctest --test-dir build --output-on-failure
```

Debug配置/构建及完整CTest **31/31通过**。随后补充完整帧golden、非零ID/摘要往返和文件上限测试，同步测试文件后再次构建，`ctest --test-dir build -R manifest_page --output-on-failure` **1/1通过**。Windows最终完整回归包含这些补充。

未运行ASan/UBSan、Release、TSan、GUI、真实增量上传或手工端到端验收。本协议子任务不需要用户手工操作；未来状态机和CLI接通后再给人工验收命令。未提交/推送。
