"""GUI owns the forwarding ssh.exe: no external -L process or shell is started.
Only the test server setup uses a separate ordinary SSH command. All data is private.
Usage: python windows_managed_ssh_test.py PROBE SSH_ALIAS SERVER_BINARY
"""
import hashlib
import json
import os
from pathlib import Path
import queue
import shlex
import subprocess
import sys
import tempfile
import threading

probe, alias, server = sys.argv[1:4]


def ssh(command):
    return subprocess.check_output(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8', alias, command],
                                   encoding='utf-8').strip()


root = ssh('mktemp -d /tmp/chunkharbor-auto-ssh-data-XXXXXXXX')
assert root.startswith('/tmp/chunkharbor-auto-ssh-data-') and root.count('/') == 2
remote = None
pid = None
try:
    remote = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', alias,
        'echo $$; exec ' + shlex.quote(server) + ' --upload --root ' + shlex.quote(root) + ' --port 0'],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8')
    lines = queue.Queue()

    def read():
        for line in remote.stdout:
            lines.put(line.strip())

    threading.Thread(target=read, daemon=True).start()
    pid = int(lines.get(timeout=15))
    listening = lines.get(timeout=15)
    assert listening.startswith('LISTENING 127.0.0.1:'), listening
    port = listening.rsplit(':', 1)[1]
    with tempfile.TemporaryDirectory(prefix='chunkharbor-auto-ssh-') as temporary:
        local = Path(temporary)
        source = local / '中文源目录'
        source.mkdir()
        (source/'empty').write_bytes(b'')
        (source/'中文.txt').write_text('自动 SSH 备份测试', encoding='utf-8')
        (source/'multi.bin').write_bytes(bytes(range(256)) * 4096)
        for mode in ('backup', 'reconnect', 'offline', 'interrupt'):
            selected_source = source
            if mode == 'interrupt':
                selected_source = local/'interrupt-source'
                selected_source.mkdir()
                (selected_source/'pending.bin').write_bytes(b'P' * (16 * 1024 * 1024))
            target_port = port
            if mode == 'offline':
                target_port = ssh("python3 -c 'import socket; s=socket.socket(); s.bind((\"127.0.0.1\",0)); print(s.getsockname()[1]); s.close()'")
            result = subprocess.run([probe, str(local/mode), str(selected_source), alias, target_port, mode],
                capture_output=True, timeout=110, env=dict(os.environ, QT_QPA_PLATFORM='offscreen'))
            output = [line[7:] for line in result.stdout.decode('utf-8').splitlines() if line.startswith('RESULT=')]
            assert output, (mode, result.returncode, result.stdout, result.stderr)
            data = json.loads(output[-1])
            assert result.returncode == 0 and data['success'] and not data['ssh_alive_after_close'], data
            assert data['starts'] == (2 if mode in ('reconnect', 'interrupt') else 1), data
            if mode == 'interrupt':
                assert data['pending_checkpoints'] > 0, data
            print('PASS', mode, json.dumps(data, ensure_ascii=True), flush=True)
        for file in source.iterdir():
            digest = ssh('sha256sum -- ' + shlex.quote(root+'/files/managed/'+file.name)).split()[0]
            assert digest == hashlib.sha256(file.read_bytes()).hexdigest()
            print('SHA256', file.name, digest, flush=True)
    print('Private server root:', root, flush=True)
finally:
    if pid and remote:
        ssh('test "$(readlink /proc/'+str(pid)+'/exe)" = ' + shlex.quote(server) + ' && kill -TERM '+str(pid))
        remote.wait(timeout=15)
