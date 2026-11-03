"""Default 30s deadlines, ~10.9MiB source, real ACKs, private SSH/server failures.

Usage: python -X utf8 THIS PROBE HOST SERVER HOOK [normal|slow|timeout|tunnel|server]
No production timeout is raised; generated data is never a user photo.
"""
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

PROBE, HOST, SERVER, HOOK = sys.argv[1:5]
mode = sys.argv[5] if len(sys.argv) > 5 else 'normal'
assert mode in ('normal', 'slow', 'timeout', 'tunnel', 'server')
delay = dict(normal=0, slow=28000, timeout=32000, tunnel=5000, server=5000)[mode]


def ssh(command):
    return subprocess.check_output(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8',
                                    HOST, command], encoding='utf-8').strip()


root = ssh('mktemp -d /tmp/chunkharbor-disconnect-matrix-XXXXXXXX')
assert root.startswith('/tmp/chunkharbor-disconnect-matrix-') and root.count('/') == 2
ssh('mkdir -m 700 ' + shlex.quote(root + '/data'))
remote = tunnel = probe = None
pid = None
remote_port = 0
with socket.socket() as reserve:
    reserve.bind(('127.0.0.1', 0))
    local_port = reserve.getsockname()[1]


def launch_server(restarted=False):
    global remote, pid, remote_port
    env = '' if restarted else ('env LD_PRELOAD=' + shlex.quote(HOOK) +
        ' CH_UPLOAD_DELAY_MARKER=' + shlex.quote(root + '/marker') +
        f' CH_UPLOAD_DELAY_MS={delay} CH_UPLOAD_DELAY_SKIP=8 ')
    command = ('echo $$; exec ' + env + shlex.quote(SERVER) + ' --upload --root ' +
               shlex.quote(root + '/data') + ' --port ' + str(remote_port))
    # Intentionally omit idle-timeout-ms: the actual default is 30 seconds.
    remote = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', HOST, command],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True, encoding='utf-8')
    lines = queue.Queue()
    stream = remote.stdout
    def read():
        for line in stream:
            lines.put(line.strip())
    threading.Thread(target=read, daemon=True).start()
    pid = int(lines.get(timeout=15))
    listening = lines.get(timeout=15)
    assert listening.startswith('LISTENING 127.0.0.1:'), listening
    remote_port = int(listening.rsplit(':', 1)[1])


def start_tunnel():
    global tunnel
    tunnel = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', '-o', 'ExitOnForwardFailure=yes',
        '-N', '-L', f'{local_port}:127.0.0.1:{remote_port}', HOST],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    until = time.monotonic() + 10
    while True:
        assert tunnel.poll() is None, 'tunnel exited'
        try:
            with socket.create_connection(('127.0.0.1', local_port), timeout=.1):
                return
        except OSError:
            assert time.monotonic() < until, 'tunnel startup timed out'
            time.sleep(.05)


def stop_server(signal):
    global pid
    ssh('test "$(readlink /proc/' + str(pid) + '/exe)" = ' + shlex.quote(SERVER) +
        ' && kill -' + signal + ' ' + str(pid))
    remote.wait(timeout=15)
    pid = None


def finish(process):
    out, err = process.communicate(timeout=85)
    lines = [line[7:] for line in out.decode('utf-8').splitlines() if line.startswith('RESULT=')]
    assert lines, (process.returncode, out, err)
    return process.returncode, json.loads(lines[-1]), out


try:
    launch_server()
    start_tunnel()
    with tempfile.TemporaryDirectory(prefix='chunkharbor-disconnect-matrix-') as temp:
        local = Path(temp)
        source = local / 'source'
        source.mkdir()
        large = source / 'a-10.9MiB.bin'
        large.write_bytes(os.urandom(109 * 1024 * 1024 // 10))
        (source / 'b-next.bin').write_bytes(os.urandom(65536))
        command = [PROBE, str(local / 'state'), str(source), str(local_port), 'matrix',
                   'backup', 'incremental']
        started = time.monotonic()
        probe = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  env=dict(os.environ, QT_QPA_PLATFORM='offscreen'))
        if mode in ('tunnel', 'server'):
            until = time.monotonic() + 25
            while ssh('test -f ' + shlex.quote(root + '/marker') + ' && echo ready || true') != 'ready':
                assert probe.poll() is None and time.monotonic() < until, 'did not reach delayed ninth block'
                time.sleep(.05)
            if mode == 'tunnel':
                tunnel.terminate()
                tunnel.wait(timeout=10)
                tunnel = None
                assert ssh('test "$(readlink /proc/' + str(pid) + '/exe)" = ' +
                           shlex.quote(SERVER) + ' && echo alive') == 'alive'
            else:
                stop_server('KILL')
        code, result, out = finish(probe)
        elapsed = time.monotonic() - started
        assert ssh('test -f ' + shlex.quote(root + '/marker') + ' && echo ready || true') == 'ready', 'hook not reached'
        rows = {row['path']: row for row in result['rows']}
        first = rows['matrix/a-10.9MiB.bin']
        second = rows['matrix/b-next.bin']
        if mode in ('normal', 'slow'):
            assert code == 0 and result['success'] and first['version'] and second['version'], result
            if mode == 'slow':
                assert elapsed >= 28, ('slow sync not observed', elapsed)
            print(f'{mode}: elapsed={elapsed:.2f}s; injection marker confirmed', flush=True)
        else:
            assert code == 2 and not result['success'] and first['status'] == '错误' and not first['version'], result
            if mode == 'timeout':
                assert 'server response timed out' in first['note'], result
                assert second['version'], result
                # The next file finishes after the delayed operation releases DB lock.
                assert elapsed >= 29, elapsed
            else:
                assert 'disconnected' in first['note'], result
                assert second['status'] == '错误' and not second['version'], result
            ack_values = [int(line[4:]) for line in out.decode('utf-8').splitlines() if line.startswith('ACK=')]
            assert ack_values and max(ack_values) >= 8 * 32768, out
            states = [json.loads(p.read_text(encoding='utf-8')) for p in
                      (local / 'state/sessions').glob('*.json')]
            checkpoint = next(s for s in states if s['identity']['path'] == first['path'])
            assert not checkpoint['completed'] and checkpoint['session'], checkpoint
            if mode == 'server':
                launch_server(restarted=True)
            elif mode == 'tunnel':
                start_tunnel()
                # Wait beyond the injected sync; it is not an upload timeout change.
                time.sleep(5)
            # Inspect metadata read-only: earlier acknowledged blocks must survive.
            db = root + '/data/metadata.sqlite3'
            code_db = ('import sqlite3; db=sqlite3.connect(' + repr('file:' + db + '?mode=ro') +
                ',uri=True); print([r[0] for r in db.execute(' +
                repr('SELECT idx FROM manifest_chunks WHERE session=? AND accepted=1 ORDER BY idx') +
                ',(' + repr(checkpoint['session']) + ',))])')
            durable = json.loads(ssh('python3 -c ' + shlex.quote(code_db)))
            assert durable[:8] == list(range(8)), durable
            print(f'{mode}: elapsed={elapsed:.2f}s; error={first["note"]}; durable_blocks={len(durable)}', flush=True)
            command[-1] = 'resume'
            probe = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                      env=dict(os.environ, QT_QPA_PLATFORM='offscreen'))
            code, result, _ = finish(probe)
            assert code == 0 and result['success'] and all(r['version'] for r in result['rows']), result
            resumed = next(r for r in result['rows'] if r['path'] == first['path'])
            sent = re.search(r'本轮发送 (\d+) 块', resumed['note'])
            expected = (large.stat().st_size + 32767) // 32768 - len(durable)
            assert sent and int(sent[1]) == expected, (durable, resumed)
            print(f'{mode}: resume sent {sent[1]} missing blocks; retained {len(durable)} durable blocks', flush=True)
        for file in source.iterdir():
            actual = ssh('sha256sum -- ' + shlex.quote(root + '/data/files/matrix/' + file.name)).split()[0]
            assert actual == hashlib.sha256(file.read_bytes()).hexdigest()
        print(f'PASS {mode}: {large.stat().st_size} bytes, both hashes match; root={root}', flush=True)
finally:
    original_failure = sys.exc_info()[0] is not None
    cleanup_errors = []
    try:
        if probe and probe.poll() is None:
            probe.kill()
            probe.wait(timeout=10)
    except Exception as error:
        cleanup_errors.append('probe: ' + str(error))
    try:
        if pid:
            stop_server('TERM')
    except Exception as error:
        cleanup_errors.append('server: ' + str(error))
    finally:
        try:
            if tunnel:
                tunnel.terminate()
                tunnel.wait(timeout=10)
        except Exception as error:
            cleanup_errors.append('tunnel: ' + str(error))
    if cleanup_errors:
        message = '; '.join(cleanup_errors)
        if original_failure:
            print('Cleanup also failed: ' + message, file=sys.stderr)
        else:
            raise RuntimeError(message)
