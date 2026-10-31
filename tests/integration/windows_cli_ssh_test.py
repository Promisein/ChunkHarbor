"""Optional developer check: Windows CLI through SSH tunnel to Ubuntu test server."""
import hashlib
import json
import os
import queue
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

CLI, REMOTE, SERVER = sys.argv[1:4]
def ssh(*args):
    return subprocess.run(['ssh', '-o', 'BatchMode=yes', REMOTE, *args],
                          capture_output=True, text=True, check=True).stdout.strip()

root = ssh('mktemp -d /tmp/chunkharbor-wincli-XXXXXXXX')
assert re.fullmatch(r'/tmp/chunkharbor-wincli-[A-Za-z0-9]{8}', root), root
messages = queue.Queue()
remote = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', REMOTE,
                           f'echo $$; exec {SERVER} --upload --root {root} --port 0'],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
def read_lines():
    for line in remote.stdout:
        messages.put(line.strip())
reader = threading.Thread(target=read_lines, daemon=True)
reader.start()
tunnel = None
pid = None
try:
    pid = int(messages.get(timeout=10))
    line = messages.get(timeout=10)
    assert line.startswith('LISTENING 127.0.0.1:'), line
    server_port = int(line.rsplit(':', 1)[1])
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        local_port = probe.getsockname()[1]
    tunnel = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', '-N',
                               '-L', f'{local_port}:127.0.0.1:{server_port}', REMOTE],
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    for _ in range(100):
        try:
            with socket.create_connection(('127.0.0.1', local_port), timeout=0.2):
                break
        except OSError:
            time.sleep(0.05)
    else:
        raise AssertionError('SSH tunnel did not start')
    with tempfile.TemporaryDirectory(prefix='chunkharbor-wincli-') as temp:
        source = Path(temp) / 'source.bin'
        state = Path(temp) / 'session.json'
        content = os.urandom(512 * 1024 + 37)
        source.write_bytes(content)
        result = subprocess.run([CLI, 'upload', '--file', str(source), '--path', 'windows-cli.bin',
                                 '--state', str(state), '--port', str(local_port),
                                 '--chunk-size', '65536'], capture_output=True, text=True, timeout=60)
        assert result.returncode == 0, (result.stdout, result.stderr)
        assert 'UPLOAD OK' in result.stdout and not state.exists(), result.stdout
        completed_id = re.search(r'NEW session=([0-9a-f]{32})', result.stdout)
        assert completed_id, result.stdout
        history = subprocess.run([CLI, 'status', '--session-id', completed_id.group(1),
                                  '--port', str(local_port)], capture_output=True, text=True, timeout=30)
        assert history.returncode == 0 and 'COMMITTED session=' in history.stdout, (history.stdout, history.stderr)
        remote_hash = ssh('sha256sum', root + '/files/windows-cli.bin').split()[0]
        assert remote_hash == hashlib.sha256(content).hexdigest()
        source2 = Path(temp) / 'resume-source.bin'
        state2 = Path(temp) / 'resume-session.json'
        resumed_content = os.urandom(4 * 1024 * 1024 + 19)
        source2.write_bytes(resumed_content)
        total_blocks = (len(resumed_content) + 65535) // 65536
        first = subprocess.Popen([CLI, 'upload', '--file', str(source2), '--path', 'windows-resume.bin',
                                  '--state', str(state2), '--port', str(local_port),
                                  '--chunk-size', '65536', '--delay-ms', '80'],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            for _ in range(100):
                if state2.exists(): break
                time.sleep(0.05)
            assert state2.exists(), 'CLI did not save the session ID'
            time.sleep(1)
        finally:
            first.terminate()
            first.communicate(timeout=10)
        status = subprocess.run([CLI, 'status', '--state', str(state2), '--port', str(local_port)],
                                capture_output=True, text=True, timeout=30)
        assert status.returncode == 0, (status.stdout, status.stderr)
        found = re.search(r'MISSING_BLOCKS=(\d+)', status.stdout)
        assert found, status.stdout
        missing = int(found.group(1))
        assert 0 < missing < total_blocks, (missing, total_blocks)
        active_id = json.loads(state2.read_text(encoding='utf-8'))['session_id']
        by_id = subprocess.run([CLI, 'status', '--session-id', active_id, '--port', str(local_port)],
                               capture_output=True, text=True, timeout=30)
        assert by_id.returncode == 0 and f'MISSING_BLOCKS={missing}' in by_id.stdout, (by_id.stdout, by_id.stderr)
        source2.write_bytes(b'!' + resumed_content[1:])
        changed = subprocess.run([CLI, 'upload', '--file', str(source2), '--path', 'windows-resume.bin',
                                  '--state', str(state2), '--port', str(local_port),
                                  '--chunk-size', '65536'], capture_output=True, text=True, timeout=30)
        assert changed.returncode != 0 and 'saved session does not match' in changed.stderr
        source2.write_bytes(resumed_content)
        resumed = subprocess.run([CLI, 'upload', '--file', str(source2), '--path', 'windows-resume.bin',
                                  '--state', str(state2), '--port', str(local_port),
                                  '--chunk-size', '65536'], capture_output=True, text=True, timeout=60)
        assert resumed.returncode == 0 and 'RESUME session=' in resumed.stdout, (resumed.stdout, resumed.stderr)
        assert f'UPLOAD OK blocks_sent_this_run={missing} ' in resumed.stdout, resumed.stdout
        assert ssh('sha256sum', root + '/files/windows-resume.bin').split()[0] == hashlib.sha256(resumed_content).hexdigest()
        print('Windows CLI upload, state query and resume over SSH passed')
finally:
    if tunnel:
        tunnel.terminate()
        tunnel.wait(timeout=10)
        tunnel.stderr.close()
    if pid:
        ssh('kill', '-TERM', str(pid))
    remote.wait(timeout=10)
    remote.stdout.close()
    remote.stderr.close()
    ssh('rm', '-rf', '--', root)
