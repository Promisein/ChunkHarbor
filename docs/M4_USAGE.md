# M4 使用与人工复现

当前支持 Linux CLI / Windows Qt CLI 单向固定块增量备份、只读预览、目录批任务和恢复；GUI仍仅为连接原型。服务端只监听loopback，经SSH开发使用。普通TCP不具备认证/TLS。

## 1. Ubuntu：创建全新测试根并启动

本轮实际验证二进制在下面的隔离目录。若虚拟机重启后临时目录不存在，先从当前源码按README重新构建，再替换二进制路径。

```bash
root=$(mktemp -d /tmp/chunkharbor-m4-accept-XXXXXXXX)
printf 'TEST_ROOT=%s\n' "$root"
/tmp/chunkharbor-m4-full-umBcFYdE/build/chunkharbor-server --upload --root "$root" --port 8787
```

预期 `LISTENING 127.0.0.1:8787`。保持终端和root变量；不要指向现有真实备份根。若8787占用，换空闲端口并同步修改隧道。

## 2. Windows终端A：保持SSH隧道

```powershell
ssh -N -L 18787:127.0.0.1:8787 miniob-ubuntu
```

## 3. Windows终端B：准备专用数据

```powershell
cd C:\Users\Jianing\Desktop\C++Project\ChunkHarbor
$env:PATH='D:\QT\Tools\mingw1310_64\bin;D:\QT\6.8.3\mingw_64\bin;'+$env:PATH
$cli = Join-Path $PWD 'build-windows-m4\chunkharbor-cli.exe'
$work = Join-Path $env:TEMP ('chunkharbor-m4-accept-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
$source = Join-Path $work 'source.bin'
$state = Join-Path $work 'session.state'
python -c "import os,sys; open(sys.argv[1],'xb').write(os.urandom(262144))" $source
```

若没有二进制，先按M4_VALIDATION中的Windows命令构建；Qt Kit路径以本机安装为准。

## 4. 首次备份，再原样备份

```powershell
& $cli backup --file $source --path manual/source.bin --state $state --port 18787 --chunk-size 65536
& $cli backup --file $source --path manual/source.bin --state $state --port 18787 --chunk-size 65536
```

两次都应 `BACKUP OK`；第一次 `blocks_sent_this_run=4`，第二次为`0`。第二次仍计算摘要和交换清单，不是完全没有网络通信。成功移除state文件。

## 5. 只改第二块一个字节，预览并备份

```powershell
$file = [System.IO.File]::Open($source, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite)
try {
    $file.Position = 65536
    $value = $file.ReadByte()
    $file.Position = 65536
    $file.WriteByte([byte]($value -bxor 1))
    $file.Flush()
} finally { $file.Dispose() }
& $cli preview --file $source --path manual/source.bin --state $state --port 18787 --chunk-size 65536
& $cli backup --file $source --path manual/source.bin --state $state --port 18787 --chunk-size 65536
(Get-FileHash -Algorithm SHA256 $source).Hash.ToLowerInvariant()
```

预览应为`Modified`、`missing_blocks=1`、`missing_bytes=65536`；预览不更新正式文件。backup应为`blocks_sent_this_run=1`。Ubuntu另开终端（将root赋值为步骤1打印的TEST_ROOT）运行：

```bash
sha256sum "$root/files/manual/source.bin"
```

两端SHA-256应一致。需要严格使用先前预览基线时，将preview输出的base_version传给backup的`--base-version ID`，同时传`--source-sha256 HASH`；源或基线变化将拒绝执行。未显式提供条件时backup按执行时快照重新协商。

## 6. 目录批任务、中文名与排除

```powershell
$directory = Join-Path $work 'files'
New-Item -ItemType Directory -Path (Join-Path $directory 'build') -Force | Out-Null
[System.IO.File]::WriteAllText((Join-Path $directory '中文.txt'), 'ChunkHarbor M4')
[System.IO.File]::WriteAllBytes((Join-Path $directory 'empty.bin'), [byte[]]@())
[System.IO.File]::WriteAllText((Join-Path $directory 'build\skip.txt'), 'excluded')
& $cli preview --directory $directory --prefix manual/batch --state-dir (Join-Path $work 'preview-state') --exclude build --port 18787
& $cli backup --directory $directory --prefix manual/batch --state-dir (Join-Path $work 'backup-state') --exclude build --port 18787
```

预期汇总`success=2 failed=0`，显示Excluded，服务端没有`manual/batch/build`，空文件存在且大小0。目录逐文件提交，不是整目录原子快照；独立空目录不保留，不传播本地删除。排除仅为精确目录组件名，不是完整gitignore语法。

取消可按Ctrl+C，等待CLI退出后重跑相同命令/状态目录；完成记录会跳过该未完成批次已提交的文件。若源被修改或删除，明确报错；失败不假报整批成功。成功整批后再运行属于一次新备份。

## 7. 真实中断、服务端SIGKILL与续传

下面的脚本会自动建立自己的隧道、创建新测试根和随机文件，只终止它自己启动且核对过归属的服务进程；不使用步骤1的服务端或备份数据：

```powershell
python tests/integration/windows_delta_ssh_test.py build-windows-m4/chunkharbor-cli.exe miniob-ubuntu /tmp/chunkharbor-m4-full-umBcFYdE/build/chunkharbor-server
```

预期最后输出`Windows M4 SSH: ... server SIGKILL resume passed`和专用TEST_ROOT。脚本已在本轮实际运行通过；保留远端专用数据供检查。它还验证端点变化拒绝、缺失块续传及最终SHA一致。

如需反馈人工结果，请保留backup/preview输出、SUMMARY、两端SHA-256、命令退出码和TEST_ROOT；遮盖个人目录即可，不要分享文件原文或私钥。

## 限制

单文件64MiB/65536块，目录最多10000个文件、深度64；串行调度。固定分块对文件头插入可能重传很多块。不变内容可复用版本身份；改变块大小会退化全量比较，不改旧版本分块元数据。历史内部版本尚无GC、浏览和总空间配额，磁盘会增长；无性能基准、物理掉电/磁盘满或WindowsGUI上传结论。
