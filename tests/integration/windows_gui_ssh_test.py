"""Real Windows Widgets/controller -> SSH -> Ubuntu server, private test roots only."""
import hashlib
import contextlib
import json
import os
from pathlib import Path
import queue
import re
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

PROBE, HOST, SERVER = sys.argv[1:4]


def ssh(command):
    return subprocess.check_output(['ssh', '-o', 'BatchMode=yes', HOST, command], encoding='utf-8').strip()


root = ssh('mktemp -d /tmp/chunkharbor-m5-data-XXXXXXXX')
assert re.fullmatch(r'/tmp/chunkharbor-m5-data-[A-Za-z0-9]{8}', root), root
remote = tunnel = process = None
pid = None


def launch(port=0):
    p = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', HOST,
                          'echo $$; exec ' + shlex.quote(SERVER) + ' --upload --root ' + root + ' --port ' + str(port)],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, encoding='utf-8')
    lines = queue.Queue()
    def reader():
        for line in p.stdout:
            lines.put(line.strip())
    threading.Thread(target=reader, daemon=True).start()
    process_id = int(lines.get(timeout=15))
    listening = lines.get(timeout=15)
    assert listening.startswith('LISTENING 127.0.0.1:'), listening
    return p, process_id, int(listening.rsplit(':', 1)[1])


def result(p):
    r = subprocess.run(p, capture_output=True, encoding='utf-8', errors='replace', timeout=95,
                       env=dict(os.environ, QT_QPA_PLATFORM='offscreen'))
    lines = [line[7:] for line in r.stdout.splitlines() if line.startswith('RESULT=')]
    assert lines, (r.returncode, r.stdout, r.stderr)
    return r.returncode, json.loads(lines[-1])


def verify(source, logical):
    expected = hashlib.sha256(source.read_bytes()).hexdigest()
    actual = ssh('sha256sum -- ' + shlex.quote(root + '/files/' + logical)).split()[0]
    assert actual == expected, (source, expected, actual)


try:
    remote, pid, remote_port = launch()
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        port = s.getsockname()[1]
    tunnel = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', '-o', 'ExitOnForwardFailure=yes', '-N',
                               '-L', f'{port}:127.0.0.1:{remote_port}', HOST],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    for _ in range(100):
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=.2):
                break
        except OSError:
            time.sleep(.05)
    else:
        raise AssertionError('SSH tunnel unavailable')
    with contextlib.nullcontext(tempfile.mkdtemp(prefix='chunkharbor-m5-gui-')) as local:
        local = Path(local)
        print('local test artifacts: ' + str(local) + '; remote test data: ' + root, flush=True)
        source = local / '中文目录'
        source.mkdir()
        file = source / 'source.bin'
        file.write_bytes(os.urandom(1024 * 1024 + 19))
        (source / '中文.txt').write_text('真实 Windows GUI 备份\n', encoding='utf-8')
        (source / 'empty').touch()
        (source / 'build').mkdir()
        (source / 'build' / 'excluded.txt').write_text('excluded', encoding='utf-8')
        def command(state='state', prefix='workspace', operation='backup', mode='incremental'):
            return [PROBE, str(local / state), str(source), str(port), prefix, operation, mode]
        code, r = result(command(operation='preview'))
        assert code == 0 and r['success'], r
        assert ssh('test ! -f ' + root + '/files/workspace/source.bin && echo READ_ONLY') == 'READ_ONLY'
        print('read-only real preview and Chinese path: passed', flush=True)
        code, r = result(command())
        assert code == 0 and r['success'] and r['history'] == 3, r
        verify(file, 'workspace/source.bin')
        verify(source / '中文.txt', 'workspace/中文.txt')
        assert ssh('test ! -f ' + root + '/files/workspace/build/excluded.txt && echo EXCLUDED') == 'EXCLUDED'
        code, r = result(command())
        assert code == 0 and all(int(x['blocks']) == 0 for x in r['rows']), r
        print('first backup, hashes, empty/excluded files, unchanged 0 blocks: passed', flush=True)
        data = bytearray(file.read_bytes()); data[40000] ^= 0x80; file.write_bytes(data)
        code, r = result(command())
        changed = next(x for x in r['rows'] if x['path'].endswith('source.bin'))
        assert code == 0 and int(changed['blocks']) == 1, r
        verify(file, 'workspace/source.bin')
        print('one changed block and exact server SHA256: passed', flush=True)
        before_change = hashlib.sha256(file.read_bytes()).hexdigest()
        code, r = result(command(operation='source-change'))
        assert code != 0 and not r['success'], r
        assert ssh('sha256sum -- '+shlex.quote(root+'/files/workspace/source.bin')).split()[0] == before_change
        print('source mutation after preview rejected: passed', flush=True)
        code, r = result(command(state='full', mode='full'))
        assert code != 0, r
        print('first-upload mode protects existing remote target: passed', flush=True)
        # Freeze one GUI after preview; another client publishes a new baseline.
        process = subprocess.Popen(command(state='conflict-a', operation='wait'),
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   encoding='utf-8', errors='replace',
                                   env=dict(os.environ, QT_QPA_PLATFORM='offscreen'))
        ready = queue.Queue()
        def read_conflict():
            for line in process.stdout:
                ready.put(line.strip())
        threading.Thread(target=read_conflict, daemon=True).start()
        assert ready.get(timeout=30) == 'PREVIEW_READY'
        alternate = local / 'alternate'
        shutil.copytree(source, alternate)
        other = alternate / 'source.bin'
        data = bytearray(other.read_bytes()); data[100] ^= 1; other.write_bytes(data)
        args = command(state='conflict-b'); args[2] = str(alternate)
        code, r = result(args); assert code == 0, r
        (local / 'conflict-a' / 'go').touch()
        conflict = None
        for _ in range(50):
            line = ready.get(timeout=15)
            if line.startswith('RESULT='):
                conflict = json.loads(line[7:]); break
        process.wait(timeout=10)
        assert conflict and not conflict['success'] and process.returncode != 0, conflict
        verify(other, 'workspace/source.bin')
        print('remote baseline changed after preview: protected newer version', flush=True)
        file.write_bytes(os.urandom(16 * 1024 * 1024))
        args = command(state='crash', prefix='crash')
        process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   encoding='utf-8', errors='replace',
                                   env=dict(os.environ, QT_QPA_PLATFORM='offscreen'))
        lines = queue.Queue()
        def consume():
            for line in process.stdout:
                lines.put(line.strip())
        threading.Thread(target=consume, daemon=True).start()
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            line = lines.get(timeout=20)
            if line.startswith('ACK='):
                process.kill(); process.wait(timeout=5)
                break
            assert not line.startswith('RESULT='), line
        else:
            raise AssertionError('no durable progress before deadline')
        ssh('kill -9 ' + str(pid)); remote.wait(timeout=10)
        remote, pid, _ = launch(remote_port)
        code, r = result(args)
        assert code == 0 and r['success'], r
        assert r['heartbeat'] > 0, r
        verify(file, 'crash/source.bin')
        print('GUI process kill + server SIGKILL + reopen/continue with exact SHA256: passed', flush=True)
        file.write_bytes(os.urandom(4 * 1024 * 1024))
        code, r = result(command(state='cancel', prefix='cancel', operation='cancel'))
        assert code != 0 and not r['success'], r
        code, r = result(command(state='cancel', prefix='cancel', mode='resume'))
        # The batch also contains a not-yet-started file: strict Resume must
        # never silently create a new upload for it. Confirm a new mixed plan.
        assert code == 2 and not r['success'] and any(
            row['status'] == '模式不匹配' and row['advice'] == 'full' for row in r['rows']), r
        code, r = result(command(state='cancel', prefix='cancel', mode='incremental'))
        assert code == 0 and r['success'], r
        verify(file, 'cancel/source.bin')
        print('pause/cancel mixed batch: strict Resume refused new file; confirmed incremental recovery passed', flush=True)
        code, r = result(command(state='close', prefix='close', operation='close'))
        assert code != 0 and not r['success'], r
        code, r = result(command(state='close', prefix='close', mode='resume'))
        assert code == 2 and not r['success'] and any(
            row['status'] == '模式不匹配' and row['advice'] == 'full' for row in r['rows']), r
        code, r = result(command(state='close', prefix='close', mode='incremental'))
        assert code == 0 and r['success'], r
        verify(file, 'close/source.bin')
        print('close window cooperatively and reopen/continue: passed', flush=True)
    print('Windows GUI -> SSH -> Ubuntu M5 integration passed; remote test root: ' + root, flush=True)
finally:
    if process is not None and process.poll() is None:
        process.kill(); process.wait(timeout=5)
    if pid:
        try:
            ssh('kill -TERM ' + str(pid))
        except subprocess.CalledProcessError:
            pass
    if remote is not None:
        try:
            remote.wait(timeout=10)
        except subprocess.TimeoutExpired:
            remote.terminate()
    if tunnel is not None:
        tunnel.terminate(); tunnel.wait(timeout=10)
