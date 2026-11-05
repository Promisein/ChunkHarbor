"""Selected scopes/rules -> real Ubuntu, isolated data; never deploys to the user's store.

Usage: python windows_scope_ssh_test.py GUI_PROBE SSH_ALIAS SERVER_BINARY
"""
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

PROBE, HOST, SERVER = sys.argv[1:4]


def ssh(command):
    return subprocess.check_output(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8',
                                    HOST, command], encoding='utf-8').strip()


root = ssh('mktemp -d /tmp/chunkharbor-scope-data-XXXXXXXX')
assert root.startswith('/tmp/chunkharbor-scope-data-') and root.count('/') == 2
remote = tunnel = None
pid = None
try:
    remote = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', HOST,
        'echo $$; exec ' + shlex.quote(SERVER) + ' --upload --root ' + shlex.quote(root) + ' --port 0'],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8')
    messages = queue.Queue()

    def read():
        for line in remote.stdout:
            messages.put(line.strip())

    threading.Thread(target=read, daemon=True).start()
    pid = int(messages.get(timeout=15))
    listening = messages.get(timeout=15)
    assert listening.startswith('LISTENING 127.0.0.1:'), listening
    with socket.socket() as reserved:
        reserved.bind(('127.0.0.1', 0))
        port = reserved.getsockname()[1]
    tunnel = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', '-o', 'ExitOnForwardFailure=yes',
        '-N', '-L', f'{port}:127.0.0.1:{listening.rsplit(":", 1)[1]}', HOST])
    deadline = time.monotonic() + 10
    while True:
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=.1):
                break
        except OSError:
            assert time.monotonic() < deadline
            time.sleep(.05)

    def sql(query):
        code = ('import sqlite3,json; print(json.dumps(sqlite3.connect(' +
                repr('file:' + root + '/metadata.sqlite3?mode=ro') +
                ',uri=True).execute(' + repr(query) + ').fetchall()))')
        return json.loads(ssh('python3 -c ' + shlex.quote(code)))

    with tempfile.TemporaryDirectory(prefix='chunkharbor-scope-') as temporary:
        local = Path(temporary)
        project = local / '中文项目'
        project.mkdir()

        def write(path, data):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            return path

        a = write(project/'src/main.cpp', b'A' * 131072)
        b = write(project/'include/main.cpp', b'B' * 128)
        doc = write(project/'README.md', b'readme')
        write(project/'build/ignored.cpp', b'never')
        write(project/'.git/config', b'never')
        write(project/'.gitignore', b'build/')
        huge = write(project/'src/unselected', b'')
        with huge.open('r+b') as file:
            file.truncate(65 * 1024 * 1024)

        def run(scope, files=(), operation='backup', state='main', prefix='scope',
                includes='', excludes='', mapping=None, scan=None, ok=True, screenshot=False):
            spec = local / ('spec-' + str(time.time_ns()) + '.json')
            spec.write_text(json.dumps(dict(scope=scope, files=[str(p) for p in files],
                mappingRoot=str(project if mapping is None else mapping), includes=includes,
                excludes=excludes, hidden=True)), encoding='utf-8')
            args = [PROBE, str(local/'states'/state), str(project if scan is None else scan),
                    str(port), prefix, operation, 'auto', '--selection', str(spec)]
            if screenshot:
                args += ['--screenshot', str(Path('build-windows-gui/scope-preview.png').resolve())]
            result = subprocess.run(args, capture_output=True, timeout=100,
                env=dict(os.environ, QT_QPA_PLATFORM='windows' if screenshot else 'offscreen'))
            output = [line[7:] for line in result.stdout.decode('utf-8').splitlines()
                      if line.startswith('RESULT=')]
            assert output, (result.returncode, result.stdout, result.stderr)
            data = json.loads(output[-1])
            assert data['success'] == ok and result.returncode == (0 if ok else 2), data
            return data

        def verify(file, prefix='scope', relative=None):
            relative = relative or file.relative_to(project).as_posix()
            digest = ssh('sha256sum -- ' + shlex.quote(root+'/files/'+prefix+'/'+relative)).split()[0]
            expected = hashlib.sha256(file.read_bytes()).hexdigest()
            assert digest == expected, (relative, digest, expected)
            print('SHA256', prefix+'/'+relative, digest, flush=True)

        def rows(data):
            return [r for r in data['rows'] if r['change'] != '已排除']

        run(1, [a])
        verify(a)
        assert sql('SELECT path FROM current_files') == [['scope/src/main.cpp']]
        run(2, [a, b, doc])
        for file in (a, b, doc):
            verify(file)
        before = sql('SELECT count(*) FROM versions')
        run(3, includes='*.cpp;*.h', excludes='include/;unselected')
        assert sql('SELECT count(*) FROM versions') == before
        assert len(rows(run(3, includes='*.cpp;*.h', excludes='include/;unselected', operation='preview', screenshot=True))) == 1
        print('PASS single/multiple same names, Unicode, no sibling traversal, rule prune and remote retention', flush=True)

        # The same mapping root keeps the existing full-upload checkpoint identity.
        # A pending writer outside an explicit selection must not be reported missing.
        resume = write(project/'data/resume.bin', b'R' * (16 * 1024 * 1024))
        run(1, [resume], operation='cancel-large', ok=False)
        assert rows(run(1, [resume], operation='preview'))[0]['strategy'] == 'resume'
        with a.open('r+b') as file:
            file.seek(40000)
            file.write(b'changed')
        new = write(project/'new.txt', b'new')
        mixed = run(2, [a, doc, resume, new], operation='preview')
        assert {r['strategy'] for r in rows(mixed)} == {'incremental', 'skip', 'resume', 'full'}, mixed
        run(2, [a, doc, resume, new])
        for file in (a, doc, resume, new):
            verify(file)
        assert sql("SELECT count(*) FROM versions WHERE path='scope/src/main.cpp'") == [[2]]
        print('PASS selected single resume + mixed full/incremental/resume/skip', flush=True)

        # A second cancelled upload is deliberately outside the next selected scope.
        other = write(project/'data/outside.bin', b'O' * (16 * 1024 * 1024))
        run(1, [other], operation='cancel-large', ok=False)
        other.unlink()
        assert rows(run(1, [doc], operation='preview'))[0]['strategy'] == 'skip'
        for operation in ('scope-change', 'mapping-change', 'rules-change', 'hidden-change', 'selection-change'):
            run(1, [a], operation=operation, ok=False)
        print('PASS plan invalidation and unrelated pending checkpoint isolation', flush=True)

        run(1, [b], prefix='mapped', mapping=project/'include')
        verify(b, prefix='mapped', relative='main.cpp')
        assert sql("SELECT count(*) FROM current_files WHERE path='scope/include/main.cpp'") == [[1]]
        legacy = local/'legacy'
        write(legacy/'file.txt', b'legacy folder')
        write(legacy/'build/no', b'excluded')
        run(0, prefix='legacy', scan=legacy, mapping=legacy)
        verify(legacy/'file.txt', prefix='legacy', relative='file.txt')
        print('PASS explicit mapping change creates a separate identity; legacy whole folder; root='+root, flush=True)
finally:
    try:
        if pid and remote:
            ssh('test "$(readlink /proc/'+str(pid)+'/exe)" = ' + shlex.quote(SERVER) +
                ' && kill -TERM ' + str(pid))
            remote.wait(timeout=15)
    finally:
        if tunnel:
            tunnel.terminate()
            tunnel.wait(timeout=10)
