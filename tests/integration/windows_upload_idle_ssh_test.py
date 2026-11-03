"""Real Windows BackupWorker through SSH against a slow-sync Ubuntu test server."""
import hashlib
import json
import os
from pathlib import Path
import queue
import shlex
import socket
import subprocess
import sys
import tempfile
import threading
import time

PROBE, HOST, SERVER, HOOK = sys.argv[1:5]
expect_disconnect = '--expect-disconnect' in sys.argv[5:]


def ssh(command):
    return subprocess.check_output(['ssh', '-o', 'BatchMode=yes', HOST, command], encoding='utf-8').strip()


root = ssh('mktemp -d /tmp/chunkharbor-upload-idle-gui-XXXXXXXX')
assert root.startswith('/tmp/chunkharbor-upload-idle-gui-') and root.count('/') == 2
ssh('mkdir -m 700 ' + shlex.quote(root + '/data'))
remote = tunnel = None
pid = None
try:
    command = ('echo $$; exec env LD_PRELOAD=' + shlex.quote(HOOK) +
               ' CH_UPLOAD_DELAY_MARKER=' + shlex.quote(root + '/marker') + ' ' +
               shlex.quote(SERVER) + ' --upload --root ' + shlex.quote(root + '/data') +
               ' --port 0 --idle-timeout-ms 1500')
    remote = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', HOST, command],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8')
    lines = queue.Queue()
    def read():
        for line in remote.stdout:
            lines.put(line.strip())
    threading.Thread(target=read, daemon=True).start()
    pid = int(lines.get(timeout=15))
    listening = lines.get(timeout=15)
    assert listening.startswith('LISTENING 127.0.0.1:'), listening
    remote_port = int(listening.rsplit(':', 1)[1])
    with socket.socket() as reserved:
        reserved.bind(('127.0.0.1', 0))
        port = reserved.getsockname()[1]
    tunnel = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', '-o', 'ExitOnForwardFailure=yes', '-N',
                               '-L', f'{port}:127.0.0.1:{remote_port}', HOST],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    until = time.monotonic() + 10
    while True:
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=.1):
                break
        except OSError:
            assert time.monotonic() < until, 'tunnel startup'
            time.sleep(.05)
    with tempfile.TemporaryDirectory(prefix='chunkharbor-upload-idle-gui-') as temp:
        local = Path(temp)
        source = local / 'source'
        source.mkdir()
        (source / 'a.bin').write_bytes(os.urandom(65536))
        (source / 'b.bin').write_bytes(os.urandom(65536))
        def run(mode):
            result = subprocess.run([PROBE, str(local / 'state'), str(source), str(port),
                                     'batch', 'backup', mode], capture_output=True, timeout=90,
                                    env=dict(os.environ, QT_QPA_PLATFORM='offscreen'))
            output = [line[7:] for line in result.stdout.decode('utf-8').splitlines() if line.startswith('RESULT=')]
            assert output, (result.returncode, result.stdout, result.stderr)
            return result.returncode, json.loads(output[-1])
        code, result = run('incremental')
        if expect_disconnect:
            errors = [r for r in result['rows'] if r['status'] == '错误']
            completed = [r for r in result['rows'] if r['status'] == '已完成']
            assert code == 2 and len(errors) == 1 and len(completed) == 1, result
            assert 'server disconnected' in errors[0]['note'] and not errors[0]['version'], result
            print('REPRODUCED: one file server disconnected, next file committed on its independent Peer', flush=True)
            code, result = run('resume')
        assert code == 0 and result['success'] and all(r['version'] for r in result['rows']), result
        for file in source.iterdir():
            actual = ssh('sha256sum -- ' + shlex.quote(root + '/data/files/batch/' + file.name)).split()[0]
            assert actual == hashlib.sha256(file.read_bytes()).hexdigest()
        print('PASS: real GUI batch/commit hashes and safe retry; isolated remote artifacts: ' + root, flush=True)
finally:
    if pid:
        ssh('test "$(readlink /proc/' + str(pid) + '/exe)" = ' + shlex.quote(SERVER) +
            ' && kill -TERM ' + str(pid))
    if tunnel:
        tunnel.terminate()
        tunnel.wait(timeout=10)
    if remote:
        remote.wait(timeout=10)
