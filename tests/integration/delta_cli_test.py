"""Real CLI/server M4 regression, isolated temporary data only."""
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import time
import signal
import threading

server, cli = sys.argv[1:3]
with tempfile.TemporaryDirectory(prefix='chunkharbor-cli-delta-') as temporary:
    root = Path(temporary)
    (root / 'server').mkdir(mode=0o700)
    process = subprocess.Popen([server, '--upload', '--root', str(root / 'server'), '--port', '0'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    lines = queue.Queue()
    threading.Thread(target=lambda: lines.put(process.stdout.readline()), daemon=True).start()
    try:
        line = lines.get(timeout=15).strip()
        assert line.startswith('LISTENING 127.0.0.1:'), line
        port = line.rsplit(':', 1)[1]
        source = root / 'source'
        source.write_bytes(b'a' * 4096 + b'b' * 4096)
        state = root / 'state'
        def run(command='backup', *extra, ok=True):
            args = [cli, command, '--port', port, '--chunk-size', '4096', *extra]
            result = subprocess.run(args, capture_output=True, text=True, timeout=45)
            assert (result.returncode == 0) == ok, (args, result.stdout, result.stderr)
            return result.stdout
        arguments = ['--file', str(source), '--path', 'file', '--state', str(state)]
        assert 'blocks_sent_this_run=2' in run('backup', *arguments)
        assert 'blocks_sent_this_run=0' in run('backup', *arguments)
        source.write_bytes(b'a' * 4096 + b'c' * 4096)
        before = (root / 'server' / 'files' / 'file').read_bytes()
        preview = run('preview', *arguments)
        assert 'Modified' in preview and 'missing_blocks=1' in preview, preview
        assert (root / 'server' / 'files' / 'file').read_bytes() == before
        assert 'blocks_sent_this_run=1' in run('backup', *arguments)
        assert (root / 'server' / 'files' / 'file').read_bytes() == source.read_bytes()
        source.write_bytes(b'a' * 4096)
        output = run('preview', *arguments)
        assert 'Modified' in output and 'missing_blocks=0' in output, output
        run('backup', *arguments)
        source.write_bytes(b'')
        output = run('preview', *arguments)
        assert 'Modified' in output and 'missing_blocks=0' in output, output
        run('backup', *arguments)
        assert 'Unchanged' in run('preview', *arguments)
        directory = root / 'directory'
        (directory / 'build').mkdir(parents=True)
        (directory / 'empty').write_bytes(b'')
        (directory / '\u4e2d\u6587.txt').write_bytes(b'unicode')
        (directory / 'build' / 'skip').write_bytes(b'excluded')
        batch = ['--directory', str(directory), '--prefix', 'batch', '--state-dir', str(directory / '.states'), '--exclude', 'build']
        output = run('backup', *batch)
        assert 'success=2' in output and 'failed=0' in output, output
        assert not (root / 'server' / 'files' / 'batch' / 'build').exists()
        assert (root / 'server' / 'files' / 'batch' / '\u4e2d\u6587.txt').read_bytes() == b'unicode'
        if os.name != 'nt':
            (directory / 'unsafe-link').symlink_to(source)
            partial = run('backup', *batch, ok=False)
            assert 'failed=1' in partial and 'success=2' in partial, partial
            assert not (root / 'server' / 'files' / 'batch' / 'unsafe-link').exists()
        if os.name != 'nt':
            fifo = root / 'fifo'
            os.mkfifo(fifo)
            rejected = subprocess.run([cli, 'backup', '--file', str(fifo), '--path', 'fifo',
                                       '--state', str(root / 'fifo-state'), '--port', port],
                                      capture_output=True, text=True, timeout=3)
            assert rejected.returncode != 0, (rejected.stdout, rejected.stderr)
            cancel_source, cancel_state = root / 'cancel-source', root / 'cancel-state'
            cancel_source.write_bytes(b'x' * (2 * 1024 * 1024))
            cancel_args = [cli, 'backup', '--file', str(cancel_source), '--path', 'cancel', '--state', str(cancel_state), '--port', port, '--chunk-size', '1024']
            job = subprocess.Popen(cancel_args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            deadline = time.monotonic() + 15
            while not cancel_state.exists() and job.poll() is None and time.monotonic() < deadline:
                time.sleep(0.005)
            assert cancel_state.exists() and job.poll() is None, 'cancellation state barrier not reached'
            job.send_signal(signal.SIGINT)
            cancelled_out, cancelled_error = job.communicate(timeout=45)
            assert job.returncode != 0 and cancel_state.exists(), (cancelled_out, cancelled_error)
            resumed = subprocess.run(cancel_args, capture_output=True, text=True, timeout=120)
            assert resumed.returncode == 0 and not cancel_state.exists(), (resumed.stdout, resumed.stderr)
            assert (root / 'server' / 'files' / 'cancel').read_bytes() == cancel_source.read_bytes()
        if os.name != 'nt':
            batch_root, batch_states = root / 'cancel-directory', root / 'cancel-directory-state'
            batch_root.mkdir()
            for index in range(3):
                (batch_root / str(index)).write_bytes(bytes([65 + index]) * (1024 * 1024))
            batch_args = [cli, 'backup', '--directory', str(batch_root), '--prefix', 'cancel-batch', '--state-dir', str(batch_states), '--port', port, '--chunk-size', '1024']
            job = subprocess.Popen(batch_args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            deadline = time.monotonic() + 45
            while not list(batch_states.glob('*.done')) and job.poll() is None and time.monotonic() < deadline:
                time.sleep(0.005)
            assert list(batch_states.glob('*.done')) and job.poll() is None, 'batch completion barrier not reached'
            job.send_signal(signal.SIGINT)
            batch_out, batch_error = job.communicate(timeout=45)
            assert job.returncode != 0 and (batch_states / '.batch').exists(), (batch_out, batch_error)
            resumed = subprocess.run(batch_args, capture_output=True, text=True, timeout=120)
            assert resumed.returncode == 0 and 'RESUME SKIP completed' in resumed.stdout, (resumed.stdout, resumed.stderr)
            assert not (batch_states / '.batch').exists()
            for index in range(3):
                assert (root / 'server' / 'files' / 'cancel-batch' / str(index)).read_bytes() == (batch_root / str(index)).read_bytes()
        # An invalid saved state is rejected before starting a delta session.
        state.write_text('invalid-endpoint-state')
        run('backup', *arguments, ok=False)
        print('delta CLI integration passed')
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
