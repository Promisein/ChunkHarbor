"""Windows Qt file-page -> SSH -> existing Ubuntu server, private data only."""
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import shlex
import socket
import subprocess
import sys
import tempfile
import threading
import time

PROBE, HOST, SERVER = sys.argv[1:4]
CLI = SERVER.rsplit('/', 1)[0] + '/chunkharbor-cli'


def ssh(command):
    return subprocess.check_output(['ssh', '-o', 'BatchMode=yes', HOST, command], encoding='utf-8').strip()


root = ssh('mktemp -d /tmp/chunkharbor-r2-1-download-XXXXXXXX')
assert re.fullmatch('/tmp/chunkharbor-r2-1-download-[A-Za-z0-9]{8}', root)
remote = tunnel = None
pid = None
try:
    remote = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', HOST,
        'echo $$; exec ' + shlex.quote(SERVER) + ' --upload --root ' + shlex.quote(root) + ' --port 0'],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8')
    lines = queue.Queue()
    threading.Thread(target=lambda: [lines.put(line.strip()) for line in remote.stdout], daemon=True).start()
    pid = int(lines.get(timeout=15))
    listening = lines.get(timeout=15)
    assert listening.startswith('LISTENING 127.0.0.1:'), listening
    remote_port = int(listening.rsplit(':', 1)[1])
    code = '''import hashlib,json,pathlib,subprocess,sys,tempfile
cli,port,root=sys.argv[1:4]
result={}
with tempfile.TemporaryDirectory(prefix="chunkharbor-r2-1-source-") as temp:
    source=pathlib.Path(temp)/"source"
    for name,data in [("empty",b""),("中文文件.txt","你好 ChunkHarbor\\n".encode()),("multi.bin",bytes(range(256))*3101)]:
        source.write_bytes(data)
        subprocess.run([cli,"upload","--port",port,"--path",name,"--file",str(source)],check=True,capture_output=True)
        saved=(pathlib.Path(root)/"files"/name).read_bytes()
        assert hashlib.sha256(data).digest()==hashlib.sha256(saved).digest()
        result[name]={"size":str(len(data)),"sha256":hashlib.sha256(saved).hexdigest()}
print(json.dumps(result))
'''
    expected = json.loads(ssh('python3 -c ' + shlex.quote(code) + ' ' +
        shlex.quote(CLI) + ' ' + str(remote_port) + ' ' + shlex.quote(root)))
    with socket.socket() as local:
        local.bind(('127.0.0.1', 0))
        port = local.getsockname()[1]
    tunnel = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', '-o', 'ExitOnForwardFailure=yes',
        '-N', '-L', f'{port}:127.0.0.1:{remote_port}', HOST], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    deadline = time.monotonic() + 10
    while True:
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=.2): break
        except OSError:
            assert time.monotonic() < deadline
            time.sleep(.05)
    with tempfile.TemporaryDirectory(prefix='chunkharbor-r2-1-restored-') as temporary:
        local = Path(temporary) / '中文恢复目录'
        local.mkdir()
        for name, entry in expected.items():
            target = local / name
            command = [PROBE, str(port), name, '12' * 16, entry['size'], entry['sha256'], str(target), 'remote']
            if len(sys.argv) > 4: command.append(str(Path(sys.argv[4]).resolve()))
            r = subprocess.run(command, capture_output=True, timeout=25,
                env=dict(os.environ, QT_QPA_PLATFORM='windows' if len(sys.argv) > 4 else 'offscreen'))
            results = [line[7:] for line in r.stdout.decode('utf-8').splitlines() if line.startswith('RESULT=')]
            assert r.returncode == 0 and results, (name, r.returncode, r.stdout, r.stderr)
            result = json.loads(results[-1])
            assert result['success'] and result['duplicate_rejected'], result
            actual = hashlib.sha256(target.read_bytes()).hexdigest()
            assert actual == entry['sha256'] and target.stat().st_size == int(entry['size'])
            assert not list(local.glob('.chunkharbor-download-*'))
            print(json.dumps(dict(path=name, size=entry['size'], server_sha256=entry['sha256'],
                                  windows_sha256=actual, result=result), ensure_ascii=True), flush=True)
    print('PASS Windows GUI SSH current download empty/UTF8/multichunk; private server root=' + root)
finally:
    if tunnel:
        tunnel.terminate()
        tunnel.communicate(timeout=10)
    if pid: ssh('kill -TERM ' + str(pid))
    if remote: remote.communicate(timeout=15)
