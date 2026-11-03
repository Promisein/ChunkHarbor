"""GUI mode/advice/preview binding against the real Ubuntu server; private data only."""
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
    return subprocess.check_output(['ssh', '-o', 'BatchMode=yes', HOST, command], encoding='utf-8').strip()


root = ssh('mktemp -d /tmp/chunkharbor-mode-XXXXXXXX')
assert root.startswith('/tmp/chunkharbor-mode-') and root.count('/') == 2
remote = tunnel = hold = None
pid = None
try:
    remote = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', HOST,
        'echo $$; exec ' + shlex.quote(SERVER) + ' --upload --root ' + shlex.quote(root) + ' --port 0'],
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
    tunnel = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', '-o', 'ExitOnForwardFailure=yes',
        '-N', '-L', f'{port}:127.0.0.1:{remote_port}', HOST], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    until = time.monotonic() + 10
    while True:
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=.1): break
        except OSError:
            assert time.monotonic() < until
            time.sleep(.05)

    def sql(query):
        code = ('import sqlite3,json; print(json.dumps(sqlite3.connect(' +
            repr('file:' + root + '/metadata.sqlite3?mode=ro') + ',uri=True).execute(' + repr(query) + ').fetchall()))')
        return json.loads(ssh('python3 -c ' + shlex.quote(code)))

    with tempfile.TemporaryDirectory(prefix='chunkharbor-mode-') as temporary:
        local = Path(temporary)
        def command(source, state, prefix, mode='incremental', operation='backup', auto=True):
            return [PROBE, str(local/'states'/state), str(source), str(port), prefix, operation, mode] + ([] if auto else ['--no-auto-resume'])

        def run(source, state, prefix, mode='incremental', operation='backup', auto=True, ok=True):
            capture = ((prefix == 'mixed' and mode == 'incremental') or
                       (prefix == 'auto-mixed' and mode == 'auto')) and operation == 'preview' and ok
            args = command(source,state,prefix,mode,operation,auto)
            if capture:
                args += ['--screenshot', str(Path('build-windows-gui/'+
                    ('auto-advice.png' if mode == 'auto' else 'mode-advice.png')).resolve())]
            result = subprocess.run(args, capture_output=True,
                timeout=90, env=dict(os.environ, QT_QPA_PLATFORM='windows' if capture else 'offscreen'))
            output = [line[7:] for line in result.stdout.decode('utf-8').splitlines() if line.startswith('RESULT=')]
            assert output, (result.returncode, result.stdout, result.stderr)
            data = json.loads(output[-1])
            assert data['success'] == ok and result.returncode == (0 if ok else 2), data
            assert data['advice_visible'], data
            return data

        def verify(source, prefix):
            for file in source.iterdir():
                digest = ssh('sha256sum -- ' + shlex.quote(root+'/files/'+prefix+'/'+file.name)).split()[0]
                assert digest == hashlib.sha256(file.read_bytes()).hexdigest()

        source = local/'single'; source.mkdir()
        file = source/'one.txt'; file.write_bytes(b'first')
        data = run(source,'fresh','single',operation='preview')
        assert data['rows'][0]['advice'] == 'full' and '首次完整上传' in data['recommendation'], data
        data = run(source,'missing','missing',mode='resume',operation='preview',auto=False,ok=False)
        assert data['rows'][0]['advice'] == 'full' and not data['rows'][0]['allowed'], data
        assert sql('SELECT count(*) FROM versions') == [[0]]
        run(source,'full','single',mode='full')
        before = sql('SELECT path,version_id FROM current_files ORDER BY path')
        versions = sql('SELECT count(*) FROM versions')
        data = run(source,'full','single',mode='full')
        assert data['rows'][0]['status'] == '已跳过' and not data['rows'][0]['version'], data
        assert sql('SELECT count(*) FROM versions') == versions
        assert sql('SELECT path,version_id FROM current_files ORDER BY path') == before
        data = run(source,'foreign','single',mode='full',operation='preview',ok=False)
        assert data['rows'][0]['advice'] == 'unchanged' and not data['rows'][0]['allowed'], data
        file.write_bytes(b'changed')
        data = run(source,'full','single',mode='full',operation='preview',ok=False)
        assert data['rows'][0]['advice'] == 'incremental', data
        run(source,'full','single',mode='incremental')
        data = run(source,'mode-change','new-target',operation='mode-change',ok=False)
        assert '重新扫描' in data['recommendation'] and sql("SELECT count(*) FROM current_files WHERE path LIKE 'new-target/%'") == [[0]], data
        verify(source,'single')
        print('PASS new/full/foreign protection/incremental/unchanged without new version/mode invalidation', flush=True)

        retry_source = local/'retry-source'; retry_source.mkdir()
        (retry_source/'a-small.txt').write_bytes(b'small')
        (retry_source/'z-pending.bin').write_bytes(os.urandom(4*1024*1024))
        data = run(retry_source,'retry-state','retry',mode='full',operation='retry-preview')
        assert sorted(row['advice'] for row in data['rows']) == ['resume','unchanged'], data
        # Retry is a new preview requiring confirmation; it must not upload B yet.
        assert sql("SELECT count(*) FROM current_files WHERE path='retry/z-pending.bin'") == [[0]]
        version_a = sql("SELECT version_id FROM current_files WHERE path='retry/a-small.txt'")
        run(retry_source,'retry-state','retry',mode='full')
        assert sql("SELECT version_id FROM current_files WHERE path='retry/a-small.txt'") == version_a
        verify(retry_source,'retry')
        print('PASS partial success then Retry refreshes preview, preserves A and confirms B separately', flush=True)

        explicit = local/'explicit-source'; explicit.mkdir()
        (explicit/'only.bin').write_bytes(os.urandom(512*1024))
        run(explicit,'explicit-state','explicit',mode='full',operation='cancel-large',ok=False)
        run(explicit,'explicit-state','explicit',mode='resume',auto=False)
        verify(explicit,'explicit')
        print('PASS explicit Resume executes even with automatic resume disabled', flush=True)

        mixed = local/'mixed'; mixed.mkdir()
        pending = mixed/'d-pending.bin'; pending.write_bytes(os.urandom(4*1024*1024))
        run(mixed,'mixed-state','mixed',mode='full',operation='cancel-large',ok=False)
        state_files = list((local/'states/mixed-state/sessions').glob('*.json'))
        checkpoint_file = next(p for p in state_files if not json.loads(p.read_text())['completed'])
        saved_bytes = checkpoint_file.read_bytes()
        data = run(mixed,'mixed-state','mixed',mode='resume',operation='preview',auto=False)
        row = data['rows'][0]
        assert row['advice'] == 'resume' and row['allowed'] and row['session'], data
        assert int(row['bytes']) < pending.stat().st_size, data
        data = run(mixed,'mixed-state','mixed',operation='preview',auto=False,ok=False)
        assert data['rows'][0]['advice'] == 'resume' and not data['rows'][0]['allowed'], data
        original = pending.read_bytes()
        pending.write_bytes(original+b'changed')
        run(mixed,'mixed-state','mixed',mode='resume',operation='preview',ok=False)
        assert checkpoint_file.read_bytes() == saved_bytes
        pending.write_bytes(original)
        pending.write_bytes(bytes([original[0] ^ 1]) + original[1:])
        run(mixed,'mixed-state','mixed',mode='resume',operation='preview',ok=False)
        assert checkpoint_file.read_bytes() == saved_bytes
        pending.write_bytes(original)
        checkpoint_file.write_bytes(b'{broken json')
        run(mixed,'mixed-state','mixed',mode='resume',operation='preview',ok=False)
        assert checkpoint_file.read_bytes() == b'{broken json'
        checkpoint_file.write_bytes(saved_bytes)
        invalid = json.loads(saved_bytes); invalid['session'] = '0'*32
        checkpoint_file.write_text(json.dumps(invalid),encoding='utf-8')
        data = run(mixed,'mixed-state','mixed',mode='resume',operation='preview',ok=False)
        assert data['rows'][0]['advice'] == 'unknown' and 'SESSION_NOT_FOUND' in data['rows'][0]['note'], data
        checkpoint_file.write_bytes(saved_bytes)
        print('PASS live resume identity/bitmap, explicit override of auto switch, source/session rejection', flush=True)

        external = local/'external'; external.mkdir()
        (external/'b-existing.txt').write_bytes(b'old')
        (external/'c-same.txt').write_bytes(b'same')
        run(external,'external','mixed',mode='full')
        (mixed/'a-new.txt').write_bytes(b'new')
        (mixed/'b-existing.txt').write_bytes(b'updated')
        (mixed/'c-same.txt').write_bytes(b'same')
        data = run(mixed,'mixed-state','mixed',operation='preview')
        assert sorted(row['advice'] for row in data['rows']) == ['full','incremental','resume','unchanged'], data
        assert data['mode'] == 'incremental' and '分类建议' in data['recommendation'], data
        unchanged_version = sql("SELECT version_id FROM current_files WHERE path='mixed/c-same.txt'")
        run(mixed,'mixed-state','mixed',mode='resume',operation='preview',ok=False)
        run(mixed,'mixed-state','mixed')
        assert sql("SELECT version_id FROM current_files WHERE path='mixed/c-same.txt'") == unchanged_version
        verify(mixed,'mixed')
        print('PASS mixed new/changed/unchanged/resumable directory; no automatic global mode change', flush=True)

        # Fixed baseline also protects a no-upload preview against a later remote update.
        hold = subprocess.Popen(command(source,'race','single',operation='wait'), stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, encoding='utf-8', env=dict(os.environ,QT_QPA_PLATFORM='offscreen'))
        ready = queue.Queue()
        stream = hold.stdout
        def consume():
            for line in stream: ready.put(line.strip())
        threading.Thread(target=consume, daemon=True).start()
        assert ready.get(timeout=30) == 'PREVIEW_READY'
        alternate = local/'alternate'; alternate.mkdir()
        (alternate/'one.txt').write_bytes(b'new remote version')
        run(alternate,'race-other','single')
        (local/'states/race/go').touch()
        while True:
            line = ready.get(timeout=30)
            if line.startswith('RESULT='):
                data = json.loads(line[7:]); break
        hold.wait(timeout=10)
        assert not data['success'] and hold.returncode == 2, data
        verify(alternate,'single')
        print('PASS remote baseline changed between unchanged preview and execution; root='+root, flush=True)

        def automatic(source, state, prefix, operation='backup', ok=True):
            # Auto must choose safe recovery independently of the manual switch.
            return run(source,state,prefix,mode='auto',operation=operation,auto=False,ok=ok)

        fresh = local/'auto-source'; fresh.mkdir()
        (fresh/'one.txt').write_bytes(b'auto first')
        data = automatic(fresh,'auto-state','auto-single','preview')
        assert data['rows'][0]['strategy'] == 'full' and data['ready'], data
        automatic(fresh,'auto-state','auto-single')
        state = next((local/'states/auto-state/sessions').glob('*.json'))
        assert json.loads(state.read_bytes())['identity']['mode'] == 'full'
        # A lost local commit receipt is historical success, not pending Resume.
        lost = json.loads(state.read_bytes()); lost['completed'] = False
        state.write_text(json.dumps(lost),encoding='utf-8')
        data = automatic(fresh,'auto-state','auto-single','preview')
        assert data['rows'][0]['strategy'] == 'skip' and json.loads(state.read_bytes())['completed'],data
        count = sql('SELECT count(*) FROM versions')
        data = automatic(fresh,'auto-state','auto-single')
        assert data['rows'][0]['strategy'] == 'skip' and not data['rows'][0]['version'], data
        assert sql('SELECT count(*) FROM versions') == count
        (fresh/'one.txt').write_bytes(b'auto changed')
        data = automatic(fresh,'auto-state','auto-single','preview')
        assert data['rows'][0]['strategy'] == 'incremental' and data['rows'][0]['base'] != 'absent', data
        automatic(fresh,'auto-state','auto-single')
        assert json.loads(state.read_bytes())['identity']['mode'] == 'incremental'
        verify(fresh,'auto-single')
        for operation in ['mode-change','prefix-change','directory-change']:
            data = automatic(fresh,'auto-config-'+operation,'auto-config-'+operation,operation,ok=False)
            assert not data['ready'] and '重新扫描' in data['recommendation'], data
        print('PASS auto full/incremental/skip; legacy checkpoint mode; all configuration invalidation',flush=True)

        mix = local/'auto-mixed'; mix.mkdir()
        (mix/'d-pending.bin').write_bytes(os.urandom(512*1024))
        # An old manually created full session remains resumable under Auto.
        run(mix,'auto-mixed-state','auto-mixed',mode='full',operation='cancel-large',ok=False)
        ext = local/'auto-external'; ext.mkdir()
        (ext/'b-change.txt').write_bytes(b'old'); (ext/'c-same.txt').write_bytes(b'same')
        run(ext,'auto-external-state','auto-mixed',mode='full')
        (mix/'a-new.txt').write_bytes(b'new')
        (mix/'b-change.txt').write_bytes(b'updated'); (mix/'c-same.txt').write_bytes(b'same')
        data = automatic(mix,'auto-mixed-state','auto-mixed','preview')
        assert sorted(r['strategy'] for r in data['rows']) == ['full','incremental','resume','skip'],data
        assert '错误 0' in data['recommendation'] and '预计上传' in data['recommendation'],data
        unchanged = sql("SELECT version_id FROM current_files WHERE path='auto-mixed/c-same.txt'")
        data = automatic(mix,'auto-mixed-state','auto-mixed')
        assert sorted(r['strategy'] for r in data['rows']) == ['full','incremental','resume','skip'],data
        assert sql("SELECT version_id FROM current_files WHERE path='auto-mixed/c-same.txt'") == unchanged
        verify(mix,'auto-mixed')
        count = sql('SELECT count(*) FROM versions')
        automatic(mix,'auto-mixed-state','auto-mixed')
        assert sql('SELECT count(*) FROM versions') == count
        print('PASS auto mixed full/incremental/resume/skip, no duplicate versions, automatic switch off',flush=True)

        broken = local/'auto-broken'; broken.mkdir()
        (broken/'bad.bin').write_bytes(os.urandom(512*1024))
        run(broken,'auto-broken-state','auto-broken',mode='full',operation='cancel-large',ok=False)
        cp = next((local/'states/auto-broken-state/sessions').glob('*.json'))
        original = cp.read_bytes()
        cp.write_bytes(b'{broken-json')
        data = automatic(broken,'auto-broken-state','auto-broken','preview',ok=False)
        assert not data['ready'],data
        (broken/'good.txt').write_bytes(b'safe file')
        data = automatic(broken,'auto-broken-state','auto-broken','preview',ok=False)
        assert data['ready'] and sorted(r['strategy'] for r in data['rows']) == ['error','full'],data
        data = automatic(broken,'auto-broken-state','auto-broken',ok=False)
        assert sorted(r['status'] for r in data['rows']) == ['已完成','错误'],data
        assert cp.read_bytes() == b'{broken-json'
        good_version = sql("SELECT version_id FROM current_files WHERE path='auto-broken/good.txt'")
        assert good_version and sql("SELECT count(*) FROM current_files WHERE path='auto-broken/bad.bin'") == [[0]]
        invalid = json.loads(original); invalid['session'] = '0'*32
        cp.write_text(json.dumps(invalid),encoding='utf-8'); invalid_bytes=cp.read_bytes()
        data = automatic(broken,'auto-broken-state','auto-broken',ok=False)
        assert cp.read_bytes() == invalid_bytes and any('SESSION_NOT_FOUND' in r['note'] for r in data['rows']),data
        cp.write_bytes(original)
        data = automatic(broken,'auto-broken-state','auto-broken')
        assert sorted(r['strategy'] for r in data['rows']) == ['resume','skip'],data
        assert sql("SELECT version_id FROM current_files WHERE path='auto-broken/good.txt'") == good_version
        verify(broken,'auto-broken')
        print('PASS corrupt/expired checkpoint isolated; safe file succeeds, bad state preserved, repair then resume',flush=True)

        retry_source = local/'auto-retry'; retry_source.mkdir()
        (retry_source/'a-small.txt').write_bytes(b'small')
        (retry_source/'z-pending.bin').write_bytes(os.urandom(512*1024))
        data = automatic(retry_source,'auto-retry-state','auto-retry','retry-preview')
        assert data['ready'] and sorted(r['strategy'] for r in data['rows']) == ['resume','skip'],data
        assert sql("SELECT count(*) FROM current_files WHERE path='auto-retry/z-pending.bin'") == [[0]]
        run(retry_source,'auto-retry-state','auto-retry',mode='resume',auto=False)
        verify(retry_source,'auto-retry')
        changed = local/'auto-source-change'; changed.mkdir()
        (changed/'source.bin').write_bytes(b'will change'); (changed/'good.txt').write_bytes(b'safe')
        data = automatic(changed,'auto-source-change','auto-source-change','source-change',ok=False)
        assert any(r['status']=='错误' and r['path'].endswith('source.bin') for r in data['rows']),data
        assert sql("SELECT count(*) FROM current_files WHERE path='auto-source-change/source.bin'") == [[0]]
        automatic(changed,'auto-source-change','auto-source-change'); verify(changed,'auto-source-change')
        print('PASS auto partial execution/source mutation then rescan; Retry requires confirmation',flush=True)

        # Another writer is blocked while a valid upload owns this target.
        pending = local/'auto-race'; pending.mkdir()
        (pending/'only.bin').write_bytes(os.urandom(512*1024))
        run(pending,'auto-race-state','auto-race',mode='full',operation='cancel-large',ok=False)
        cp = next((local/'states/auto-race-state/sessions').glob('*.json')); original=cp.read_bytes()
        session = json.loads(original)['session']
        accepted = sql("SELECT count(*) FROM manifest_chunks WHERE session='"+session+"' AND accepted=1")
        hold = subprocess.Popen(command(pending,'auto-race-state','auto-race',mode='auto',operation='wait',auto=False),
            stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8',env=dict(os.environ,QT_QPA_PLATFORM='offscreen'))
        messages=queue.Queue(); stream=hold.stdout
        def read_hold():
            for line in stream: messages.put(line.strip())
        threading.Thread(target=read_hold,daemon=True).start()
        assert messages.get(timeout=30)=='PREVIEW_READY'
        newer=local/'auto-newer'; newer.mkdir(); (newer/'only.bin').write_bytes(b'new current version')
        data = run(newer,'auto-newer','auto-race',mode='full',ok=False)
        assert any('CONFLICT' in r['note'] for r in data['rows']),data
        invalid = json.loads(original); invalid['session'] = '0'*32
        cp.write_text(json.dumps(invalid),encoding='utf-8'); invalid_bytes=cp.read_bytes()
        (local/'states/auto-race-state/go').touch()
        line=messages.get(timeout=30)
        assert line.startswith('RESULT='),line
        data=json.loads(line[7:]); hold.wait(timeout=10)
        assert not data['success'] and hold.returncode==2 and data['rows'][0]['strategy']=='error',data
        assert sql("SELECT count(*) FROM manifest_chunks WHERE session='"+session+"' AND accepted=1") == accepted
        assert cp.read_bytes()==invalid_bytes
        cp.write_bytes(original)
        automatic(pending,'auto-race-state','auto-race'); verify(pending,'auto-race')
        print('PASS active target protects against second writer; session changed after preview refused before DATA',flush=True)

        # With no pending writer, a genuine remote commit invalidates an old Skip.
        hold = subprocess.Popen(command(fresh,'auto-current-race','auto-single',mode='auto',operation='wait'),
            stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8',env=dict(os.environ,QT_QPA_PLATFORM='offscreen'))
        messages=queue.Queue(); stream=hold.stdout
        threading.Thread(target=read_hold,daemon=True).start()
        assert messages.get(timeout=30)=='PREVIEW_READY'
        (newer/'one.txt').write_bytes(b'new remote content'); (newer/'only.bin').unlink()
        automatic(newer,'auto-current-new','auto-single')
        (local/'states/auto-current-race/go').touch()
        line=messages.get(timeout=30)
        assert line.startswith('RESULT='),line
        data=json.loads(line[7:]); hold.wait(timeout=10)
        assert not data['success'] and hold.returncode==2 and data['rows'][0]['strategy']=='error',data
        verify(newer,'auto-single')
        print('PASS auto remote version changed after preview; root='+root,flush=True)
finally:
    try:
        if hold and hold.poll() is None: hold.kill(); hold.wait(timeout=10)
        if pid:
            ssh('test "$(readlink /proc/'+str(pid)+'/exe)" = '+shlex.quote(SERVER)+' && kill -TERM '+str(pid))
            remote.wait(timeout=15)
    finally:
        if tunnel: tunnel.terminate(); tunnel.wait(timeout=10)
