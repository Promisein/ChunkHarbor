"""Run the Windows history UI against a migrated copy of the v2 backup."""
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

probe, host, server, fixture = sys.argv[1:5]
ssh = ['ssh', '-o', 'BatchMode=yes', host]
mode = sys.argv[5] if len(sys.argv) > 5 else ''
root = subprocess.check_output(ssh + ['mktemp -d /tmp/chunkharbor-history-ssh-XXXXXXXX'], text=True).strip()
assert re.fullmatch(r'/tmp/chunkharbor-history-ssh-[A-Za-z0-9]{8}', root)
remote = tunnel = None
pid = None
try:
    subprocess.run(ssh + ['tar -xzf ' + shlex.quote(fixture) + ' -C ' + root], check=True)
    remote = subprocess.Popen(ssh + ['echo $$; exec ' + shlex.quote(server) +
                              ' --upload --root ' + root + '/storage --port 0'],
                              stdout=subprocess.PIPE, text=True)
    lines = queue.Queue()

    def read_lines():
        for line in remote.stdout:
            lines.put(line.strip())

    threading.Thread(target=read_lines, daemon=True).start()
    pid = int(lines.get(timeout=15))
    listening = lines.get(timeout=15)
    assert listening.startswith('LISTENING 127.0.0.1:'), listening
    remote_port = int(listening.rsplit(':', 1)[1])
    with socket.socket() as local:
        local.bind(('127.0.0.1', 0))
        port = local.getsockname()[1]
    tunnel = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', '-o', 'ExitOnForwardFailure=yes',
                               '-N', '-L', f'{port}:127.0.0.1:{remote_port}', host])
    deadline = time.monotonic() + 10
    while True:
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=.2):
                break
        except OSError:
            assert time.monotonic() < deadline, 'SSH tunnel did not start'
            time.sleep(.05)
    env = dict(os.environ, QT_QPA_PLATFORM='offscreen')
    if mode == '--restore':
        code = '''import json,sqlite3,sys
db=sqlite3.connect("file:"+sys.argv[1]+"?mode=ro",uri=True)
print(json.dumps(db.execute("SELECT id,size,sha256 FROM versions WHERE path=? ORDER BY rowid",
                           ("fixture/file.txt",)).fetchall()))
'''
        entries = json.loads(subprocess.check_output(
            ssh + ['python3 -c ' + shlex.quote(code) + ' ' + root + '/storage/metadata.sqlite3'],
            encoding='utf-8'))
        assert len(entries) == 3
        with tempfile.TemporaryDirectory(prefix='chunkharbor-r23-restored-') as tmp:
            for label, (version, size, sha) in zip('ABC', entries):
                path = Path(tmp) / f'恢复-{label}.txt'
                args = [probe, str(port), 'fixture/file.txt', version, str(size), sha, str(path), 'history_remote']
                run_env = env
                if label == 'A' and len(sys.argv) > 6:
                    args.append(str(Path(sys.argv[6]).resolve()))
                    run_env = dict(env, QT_QPA_PLATFORM='windows')
                result = subprocess.run(args, env=run_env, check=True, timeout=25,
                                        capture_output=True, encoding='utf-8')
                rows = [json.loads(line[7:]) for line in result.stdout.splitlines() if line.startswith('RESULT=')]
                assert len(rows) == 1 and rows[0]['success'] and rows[0]['duplicate_rejected'], result.stdout
                data = path.read_bytes()
                digest = hashlib.sha256(data).hexdigest()
                assert data == f'R2.2 fixture {label}'.encode() and len(data) == size and digest == sha
                print(f'{label}: {version}, {size} bytes, SHA-256 {digest}')
            assert not list(Path(tmp).glob('.chunkharbor-download-*'))
    elif mode == '--current-folder':
        from directory_snapshot_test import connect, entries, listing
        from delta_protocol_test import HEADER, exact
        cli = server.rsplit('/', 1)[0] + '/chunkharbor-cli'

        def publish(label, logical_path, content):
            source, state = root + '/' + label, root + '/' + label + '-state'
            command = [cli, 'backup', '--file', source, '--path', logical_path,
                       '--state', state, '--port', str(remote_port)]
            subprocess.run(ssh + ["printf %s " + shlex.quote(content) + " > " + shlex.quote(source)
                                  + ' && ' + shlex.join(command)], check=True, timeout=20)

        publish('nested', 'fixture/\u5b50\u76ee\u5f55/\u6587\u4ef6.txt', 'nested')
        publish('sibling', 'fixture-extra/excluded.txt', 'excluded')
        # Delay the first manifest pull and the first chunk pull to commit changes
        # on the real server at deterministic boundaries of the Windows operation.
        updates = {42: ('during-manifest', 'DDDD'), 33: ('during-download', 'EEEE')}
        errors = queue.Queue()
        stop = threading.Event()
        workers = []

        def relay(client):
            try:
                with client, socket.create_connection(('127.0.0.1', port), timeout=10) as upstream:
                    client.settimeout(10)
                    while True:
                        try:
                            first = client.recv(1)
                        except ConnectionResetError:
                            return
                        if not first:
                            return
                        header = first + exact(client, HEADER.size - 1)
                        _, _, kind, _, _, size, _ = HEADER.unpack(header)
                        assert size <= 65536
                        body = exact(client, size)
                        update = updates.pop(kind, None)
                        if update:
                            publish(update[0], 'fixture/file.txt', update[1])
                        upstream.sendall(header + body)
                        reply = exact(upstream, HEADER.size)
                        size = HEADER.unpack(reply)[5]
                        assert size <= 1024 * 1024
                        client.sendall(reply + exact(upstream, size))
            except Exception as error:
                errors.put(error)

        with socket.socket() as proxy:
            proxy.bind(('127.0.0.1', 0))
            proxy.listen()
            proxy.settimeout(.1)

            def accept():
                while not stop.is_set():
                    try:
                        client, _ = proxy.accept()
                    except socket.timeout:
                        continue
                    worker = threading.Thread(target=relay, args=(client,), daemon=True)
                    workers.append(worker)
                    worker.start()

            acceptor = threading.Thread(target=accept, daemon=True)
            acceptor.start()
            try:
                subprocess.run([probe, str(proxy.getsockname()[1]), '--download-folder'], env=env,
                               check=True, timeout=40)
            finally:
                stop.set()
                acceptor.join(2)
                for worker in workers:
                    worker.join(2)
        assert not updates and errors.empty(), (updates, list(errors.queue))
        peer = connect(port)
        try:
            snapshots = listing(peer)['snapshots']
            assert len(snapshots) == 1 and snapshots[0]['file_count'] == 3
            rows = entries(peer, snapshots[0]['snapshot_id'])
            assert [(e['path'], e['sha256']) for e in rows] == [
                ('file.txt', hashlib.sha256(b'R2.2 fixture C').hexdigest()),
                ('legacy.txt', hashlib.sha256(b'legacy fixture').hexdigest()),
                ('\u5b50\u76ee\u5f55/\u6587\u4ef6.txt', hashlib.sha256(b'nested').hexdigest())]
        finally:
            peer.close()
    elif mode in ('--snapshot', '--snapshot-preview', '--snapshot-restore'):
        cli = server.rsplit('/', 1)[0] + '/chunkharbor-cli'
        smoke = probe if mode == '--snapshot' else str(Path(__file__).with_name('directory_snapshot_test.py'))
        subprocess.run([sys.executable, smoke, server, cli, fixture, '--remote', str(port)],
                       check=True, timeout=20)
        if mode in ('--snapshot-preview', '--snapshot-restore'):
            # Publish a different current version after capturing the fixture snapshot.
            source, state = root + '/new-source', root + '/new-state'
            command = [cli, 'backup', '--file', source, '--path', 'fixture/file.txt',
                       '--state', state, '--port', str(remote_port)]
            subprocess.run(ssh + ['printf DDDD > ' + shlex.quote(source) + ' && ' + shlex.join(command)],
                           check=True, timeout=20)
            arguments = [probe, str(port)]
            if mode == '--snapshot-restore':
                from directory_snapshot_test import connect, create
                peer = connect(port)
                try:
                    newer = create(peer)
                    assert newer['file_count'] == 2
                    print('Newer snapshot', newer['snapshot_id'], flush=True)
                finally:
                    peer.close()
                arguments.append('--restore-snapshot')
            subprocess.run(arguments, env=env, check=True, timeout=40)
    else:
        subprocess.run([probe, str(port)], env=env, check=True, timeout=20)
    print('Isolated migrated copy:', root)
finally:
    if tunnel:
        tunnel.terminate()
        tunnel.wait(timeout=10)
    if pid:
        subprocess.run(ssh + [f'test "$(readlink /proc/{pid}/exe)" = {shlex.quote(server)} && kill -TERM {pid}'],
                       check=False)
    if remote:
        remote.wait(timeout=15)
