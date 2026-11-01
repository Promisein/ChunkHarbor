"""Windows CLI -> SSH -> real Linux server, private data and explicit ownership."""
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

CLI, HOST, SERVER = sys.argv[1:4]


def ssh(command):
    return subprocess.run(['ssh', '-o', 'BatchMode=yes', HOST, command],
                          capture_output=True, text=True, check=True).stdout.strip()


root = ssh('mktemp -d /tmp/chunkharbor-m4-win-XXXXXXXX')
assert re.fullmatch(r'/tmp/chunkharbor-m4-win-[A-Za-z0-9]{8}', root), root
remote = None
tunnel = None
pid = None


def launch():
    process = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', HOST,
                                'echo $$; exec ' + shlex.quote(SERVER) + ' --upload --root ' + root + ' --port 0'],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    lines = queue.Queue()

    def read():
        for line in process.stdout:
            lines.put(line.strip())

    threading.Thread(target=read, daemon=True).start()
    process_id = int(lines.get(timeout=15))
    listening = lines.get(timeout=15)
    assert listening.startswith('LISTENING 127.0.0.1:'), listening
    return process, process_id, int(listening.rsplit(':', 1)[1])


def stop_remote(crash=False):
    # PID came directly from this owned SSH child; validate its root before kill.
    script = ('import os,signal; p=' + str(pid) + '; '
              'a=open("/proc/%d/cmdline"%p,"rb").read().split(bytes([0])); '
              'assert ' + repr(root.encode()) + ' in a; '
              'os.kill(p,signal.SIGKILL if ' + repr(crash) + ' else signal.SIGTERM)')
    ssh('python3 -c ' + shlex.quote(script))
    remote.communicate(timeout=15)


def connect_tunnel(server_port):
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        local = probe.getsockname()[1]
    process = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', '-o', 'ExitOnForwardFailure=yes',
                                '-N', '-L', f'{local}:127.0.0.1:{server_port}', HOST],
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    for _ in range(100):
        try:
            with socket.create_connection(('127.0.0.1', local), timeout=.2):
                return process, local
        except OSError:
            if process.poll() is not None:
                raise AssertionError(process.stderr.read())
            time.sleep(.05)
    raise AssertionError('tunnel unavailable')


try:
    remote, pid, server_port = launch()
    tunnel, port = connect_tunnel(server_port)
    with tempfile.TemporaryDirectory(prefix='chunkharbor-m4-win-') as temp:
        local = Path(temp)
        source = local / 'source.bin'
        state = local / 'state'
        source.write_bytes(os.urandom(256 * 1024))

        def run(command='backup', ok=True, **options):
            size = options.pop('chunk_size', '65536')
            args = [CLI, command, '--port', str(port), '--chunk-size', str(size)]
            for key, value in options.items():
                args.extend(['--' + key.replace('_', '-'), str(value)])
            result = subprocess.run(args, capture_output=True, text=True, encoding='utf-8', timeout=90)
            assert (result.returncode == 0) == ok, (args, result.stdout, result.stderr)
            return result.stdout

        options = dict(file=source, path='windows/source.bin', state=state)
        assert 'blocks_sent_this_run=4' in run(**options)
        assert 'blocks_sent_this_run=0' in run(**options)
        content = bytearray(source.read_bytes())
        content[65536] ^= 1
        source.write_bytes(content)
        preview = run('preview', **options)
        assert 'Modified' in preview and 'missing_blocks=1' in preview, preview
        assert 'blocks_sent_this_run=1' in run(**options)
        assert ssh('sha256sum ' + root + '/files/windows/source.bin').split()[0] == hashlib.sha256(content).hexdigest()
        directory = local / '目录'
        directory.mkdir()
        (directory / '中文.txt').write_bytes(b'unicode')
        (directory / 'empty').write_bytes(b'')
        output = run(directory=directory, prefix='windows/batch', state_dir=local / 'batch-state')
        assert 'success=2' in output and 'failed=0' in output, output
        # Wait for real durable data, then interrupt BOTH client and server.
        large = local / 'resume.bin'
        large.write_bytes(os.urandom(16 * 1024 * 1024))
        resume_state = local / 'resume-state'
        first = subprocess.Popen([CLI, 'backup', '--port', str(port), '--chunk-size', '4096',
                                  '--file', str(large), '--path', 'windows/resume.bin', '--state', str(resume_state)],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        count = 0
        try:
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                if resume_state.exists():
                    sid = resume_state.read_text(encoding='utf-8').splitlines()[-1].split()[0]
                    assert re.fullmatch('[0-9a-f]{32}', sid)
                    code = ('import sqlite3; d=sqlite3.connect(' + repr(root + '/metadata.sqlite3') + '); '
                            'print(d.execute("SELECT count(*) FROM manifest_chunks WHERE session=? AND accepted=1",(' + repr(sid) + ',)).fetchone()[0])')
                    count = int(ssh('python3 -c ' + shlex.quote(code)))
                    if 0 < count < 4096:
                        break
                if first.poll() is not None:
                    break
                time.sleep(.02)
            assert 0 < count < 4096, (count, first.poll())
        finally:
            if first.poll() is None:
                first.terminate()
            first.communicate(timeout=10)
        stop_remote(crash=True)
        pid = None
        tunnel.terminate()
        tunnel.communicate(timeout=10)
        remote, pid, server_port = launch()
        tunnel, port = connect_tunnel(server_port)
        # Saved endpoint is part of identity; reuse the original local port.
        # Update only the test's tunnel: CLI must never silently rewrite state.
        # A new local port correctly causes rejection first.
        out = run(ok=False, file=large, path='windows/resume.bin', state=resume_state)
        saved = resume_state.read_text(encoding='utf-8').splitlines()[1]
        old_port = int(saved.split()[1])
        tunnel.terminate()
        tunnel.communicate(timeout=10)
        tunnel = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', '-o', 'ExitOnForwardFailure=yes', '-N',
                                   '-L', f'{old_port}:127.0.0.1:{server_port}', HOST], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        port = old_port
        time.sleep(.2)
        output = run(file=large, path='windows/resume.bin', state=resume_state, chunk_size='4096')
        sent = re.search(r'blocks_sent_this_run=(\d+)', output)
        assert sent and 0 < int(sent.group(1)) < 4096, output
        assert not resume_state.exists()
        assert ssh('sha256sum ' + root + '/files/windows/resume.bin').split()[0] == hashlib.sha256(large.read_bytes()).hexdigest()
        print('Windows M4 SSH: full/zero/one block, preview, Unicode directory, client interruption and server SIGKILL resume passed')
finally:
    if tunnel and tunnel.poll() is None:
        tunnel.terminate()
        tunnel.communicate(timeout=10)
    if pid and remote and remote.poll() is None:
        stop_remote()
    # Retain this private remote test directory for inspecting results; no real backup touched.
    print('TEST_ROOT=' + root)
