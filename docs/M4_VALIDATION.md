# M4 验证记录（2026-11-01）

本轮用户授权完成M4所有剩余任务；未跨入M5–M7，未提交/推送。自动测试只使用新建临时目录；没有迁移真实用户备份数据。人工复现见[M4_USAGE.md](M4_USAGE.md)。

最终配置/构建及测试：Ubuntu Debug **44/44**、Release **44/44**、ASan/UBSan **42/42**，Windows **17/17**，均通过。Windows经SSH的真实验收也通过。

## 功能与证据

| 范围 | 证据 |
| --- | --- |
| 严格协议与分页 | delta_control/plan_page/manifest_page测试、相同Windows/Linux golden、65536块/128页、32768区间/64页 |
| 单文件增量 | delta_protocol集成：首次全量、相同0块、改1块、增长/缩短/空文件、2048块交错差异跨页 |
| 版本与旧兼容 | schema v2、CAS ABA、旧M3上传→增量覆盖→重开查询旧会话；M1–M3完整回归 |
| 数据确认与错误 | 新块同时匹配manifest和数据摘要；SQLite确认触发器失败不能ACK；ENOSPC、fdatasync、对象fsync和发布目录fsync注入 |
| 崩溃恢复 | native测试rename屏障SIGKILL；LD_PRELOAD测试对象同步后、最新副本rename前/后、COMMITTED清理后四个精确屏障SIGSTOP→SIGKILL |
| 预览 | 与上传同算法同基线；Added/Modified/Unchanged；缩短/截空即使0块仍Modified；不写正式数据，12个连续预览不耗尽8个活跃名额 |
| 客户端恢复与拒绝 | 恶意peer的request/session/DONE/结果错误拒绝、状态写失败拒绝；源变化/端点变化拒绝；缺失位图仅补一块 |
| 目录批次 | Linux真实SIGINT、重启跳过完成文件、中文名/空文件/排除、链接及部分失败汇总；源删除不报全成功 |
| Windows真实SSH | 首次/0块/1块、预览、中文目录、Windows客户端中断+Ubuntu SIGKILL、同一源只补缺失块且SHA一致 |

## 执行环境与命令

Ubuntu源码隔离目录：`/tmp/chunkharbor-m4-full-umBcFYdE`，GCC11.4、CMake3.22.1、OpenSSL3、SQLite3.37.2；沿用已有私有开发头，不安装/修改系统依赖。

```bash
cd /tmp/chunkharbor-m4-full-umBcFYdE
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DSQLite3_INCLUDE_DIR=/home/ning/ChunkHarbor-m3-verify/deps/sqlite/usr/include \
  -DSQLite3_LIBRARY=/usr/lib/x86_64-linux-gnu/libsqlite3.so.0 \
  -DOPENSSL_INCLUDE_DIR=/home/ning/ChunkHarbor/build/deps/openssl/usr/include \
  -DOPENSSL_CRYPTO_LIBRARY=/usr/lib/x86_64-linux-gnu/libcrypto.so.3 \
  -DCMAKE_CXX_FLAGS=-I/home/ning/ChunkHarbor/build/deps/openssl/usr/include/x86_64-linux-gnu
cmake --build build -j 2
ctest --test-dir build --output-on-failure
```

ASan/UBSan用相同依赖参数、`-B build-asan -DCHUNKHARBOR_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug`，再构建并运行：

```bash
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-asan --output-on-failure
```

Release用`-B build-release -DCMAKE_BUILD_TYPE=Release`及相同依赖参数，随后构建/CTest。新增使用assert的测试显式`-UNDEBUG`，Release也执行其断言。LD_PRELOAD错误/暂停hook不与sanitizer组合，因此ASan比Debug/Release少2项；ASan仍有native rename SIGKILL及协议重启恢复测试。

Windows使用Qt6.8.3匹配MinGW13.1/Ninja：

```powershell
$env:PATH='D:\QT\Tools\mingw1310_64\bin;D:\QT\6.8.3\mingw_64\bin;'+$env:PATH
cmake -S . -B build-windows-m4 -G Ninja -DCMAKE_MAKE_PROGRAM='D:/CLion 2023.3.6/bin/ninja/win/x64/ninja.exe' -DCMAKE_CXX_COMPILER='D:/QT/Tools/mingw1310_64/bin/g++.exe' -DCMAKE_PREFIX_PATH='D:/QT/6.8.3/mingw_64' -DCHUNKHARBOR_BUILD_SERVER=OFF -DCHUNKHARBOR_BUILD_CLI=ON -DCHUNKHARBOR_BUILD_GUI=OFF -DCHUNKHARBOR_BUILD_HASH=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build-windows-m4 -j 2
ctest --test-dir build-windows-m4 --output-on-failure
python tests/integration/windows_delta_ssh_test.py build-windows-m4/chunkharbor-cli.exe miniob-ubuntu /tmp/chunkharbor-m4-full-umBcFYdE/build/chunkharbor-server
```

Windows配置/构建及CTest **17/17通过**，真实SSH脚本通过，脚本测试根为`/tmp/chunkharbor-m4-win-ll0a8yan`。Linux最终数量与结果以PROGRESS本轮汇总为准；构建日志在隔离目录`*-config.log`、`*-build.log`及各CTest的Testing/Temporary/LastTest.log。

## 过程与修复记录

- 首个manifest模块、FrameCodec扩展、planner消费边界、change_kind契约、FIFO拒绝和不可变分块几何等有实际先失败后通过记录；部分并行模块先实现后补测试，不能把本轮整体称为严格TDD。
- 第一次Linux全回归发现旧M3已提交文件丢失检查被历史对象掩盖，增加current_files最新副本启动核验后恢复通过。
- 独立审阅发现发布临时名可能碰撞合法用户文件，移入受限内部目录并补回归；预览缩短分类和完成preview占满名额也已修复。
- 最终独立审阅发现“内容不变但块大小改变”污染旧version_chunks，真实协议/SQLite断言先复现失败，改为仅新版本写清单后通过，并经复审确认。
- Linux新客户端FIFO打开曾超时3秒，新增O_NONBLOCK后真实CLI回归通过。
- Windows首次复用同一构建目录被两个构建并发写入导致损坏对象链接失败；切换独立build-windows-m4并串行管理该构建目录后通过，不作为源码成功证据。

## 边界

未执行TSan、物理掉电、真实硬件磁盘满、吞吐基准、MSVC、GUI上传或打包。故障注入和SIGKILL不代表硬件掉电保证；未穷举全部I/O失败排列。旧版本对象和最新副本为独立完整文件，不宣称物理去重或性能收益；历史对象无自动GC。v2库不兼容旧二进制，真实旧根升级应先另行备份和审核。
