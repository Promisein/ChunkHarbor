"""A.5 lifecycle regressions and explicit residual-resource audit."""
import concurrent.futures
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile

import delta_protocol_test as wire
from delta_plan_lock_test import prepare, finish, wait_for


def age(root, sid):
    with sqlite3.connect(root / 'metadata.sqlite3') as db:
        db.execute('UPDATE delta_sessions SET created=0 WHERE id=?', (sid,))


def metadata(path):
    return dict(path=path, size=8, mtime_ns=0, chunk_size=4, sha256=wire.digest(b'aaaaxxxx'))


def main():
    scenarios = ('expired_path', 'cleanup_retry', 'cleanup_sql', 'committed_cleanup', 'active_plan',
                 'active_materialize', 'active_query', 'restart_repeat', 'cycles', 'orphan_audit',
                 'legacy_audit', 'recovery_failure')
    for scenario in scenarios:
        with tempfile.TemporaryDirectory(prefix='chunkharbor-a5-') as tmp:
            root = Path(tmp) / 'backup'
            root.mkdir(mode=0o700)
            gate = Path(tmp) / 'gate'
            release = Path(str(gate) + '.release')
            cleanup = Path(tmp) / 'cleanup'
            env = dict(os.environ, LD_PRELOAD=sys.argv[2], CH_PLAN_TEST_GATE=str(gate),
                       CH_QUERY_TEST_GATE=str(gate), CH_CLEANUP_TEST_GATE=str(cleanup))
            server, port = wire.start(root, env)
            peers = []
            pool = concurrent.futures.ThreadPoolExecutor(max_workers=1)
            def connect():
                peer = wire.Peer(port)
                peers.append(peer)
                return peer
            try:
                if scenario == 'expired_path':
                    old = wire.begin(connect(), 'a', b'aaaaxxxx')
                    age(root, old['session_id'])
                    wire.begin(connect(), 'other', b'data')
                    new = wire.begin(connect(), 'a', b'aaaaxxxx')
                    assert connect().ask(24, dict(metadata=metadata('a'), mode='upload', base_version=None), 15)['code'] == 'CONFLICT'
                    with sqlite3.connect(root / 'metadata.sqlite3') as db:
                        assert db.execute('SELECT count(*) FROM delta_sessions WHERE id=?',
                                          (old['session_id'],)).fetchone()[0] == 0
                        assert db.execute('SELECT phase FROM delta_sessions WHERE id=?',
                                          (new['session_id'],)).fetchone()[0] == 0
                elif scenario.startswith('active_'):
                    _, original, _ = wire.upload(connect(), 'a', b'aaaabbbb', [[0, 2]])
                    p = connect()
                    ready = prepare(p, 'a', b'aaaaxxxx', mode='upload')
                    sid = ready['session_id']
                    if scenario != 'active_plan':
                        finish(p, ready)
                        p.ask(27, dict(session_id=sid, page_index=0), 20)
                        if scenario == 'active_query':
                            p.ask(27, dict(session_id=sid, page_index=1), 21)
                    gate.write_text(sid if scenario == 'active_query' else original['version_id'])
                    if scenario == 'active_plan':
                        task = pool.submit(finish, p, ready)
                    elif scenario == 'active_materialize':
                        task = pool.submit(p.ask, 27, dict(session_id=sid, page_index=1), 21)
                    else:
                        task = pool.submit(p.ask, 28, dict(session_id=sid), 29)
                    wait_for(lambda: Path(str(gate) + '.entered').exists(), 'active I/O not entered')
                    age(root, sid)
                    wire.begin(connect(), 'other', b'data')
                    with sqlite3.connect(root / 'metadata.sqlite3') as db:
                        assert db.execute('SELECT phase FROM delta_sessions WHERE id=?', (sid,)).fetchone()[0] != 7
                    if scenario != 'active_plan':
                        assert (root / '.delta' / sid).exists()
                    release.touch()
                    task.result(timeout=3)
                    assert connect().ask(28, dict(session_id=sid), 15)['code'] == 'SESSION_NOT_FOUND'
                    wire.begin(connect(), 'reap', b'data')
                    assert not (root / '.delta' / sid).exists()
                    with sqlite3.connect(root / 'metadata.sqlite3') as db:
                        assert db.execute('SELECT count(*) FROM delta_sessions WHERE id=?', (sid,)).fetchone()[0] == 0
                elif scenario in ('cleanup_retry', 'cleanup_sql', 'committed_cleanup'):
                    p = connect()
                    ready = wire.begin(p, 'a', b'aaaabbbb')
                    sid = ready['session_id']
                    wire.manifest(p, ready, b'aaaabbbb')
                    wire.plan(p, ready, [[0, 2]])
                    cleanup.write_text(sid)
                    if scenario == 'cleanup_sql':
                        with sqlite3.connect(root / 'metadata.sqlite3') as db:
                            db.execute("CREATE TRIGGER reject_cleanup BEFORE DELETE ON delta_sessions "
                                       "BEGIN SELECT RAISE(ABORT,'cleanup test'); END")
                    else:
                        Path(str(cleanup) + '.fail').touch()
                    if scenario in ('cleanup_retry', 'cleanup_sql'):
                        (root / '.delta' / ('.publish-' + sid)).write_bytes(b'partial')
                        age(root, sid)
                        assert connect().ask(24, dict(metadata=metadata('other'), mode='upload', base_version=None), 15)['code'] == 'INTERNAL_ERROR'
                        with sqlite3.connect(root / 'metadata.sqlite3') as db:
                            assert db.execute('SELECT phase FROM delta_sessions WHERE id=?', (sid,)).fetchone()[0] == 7
                            if scenario == 'cleanup_sql':
                                assert db.execute('SELECT count(*) FROM manifest_chunks WHERE session=?', (sid,)).fetchone()[0] == 2
                                db.execute('DROP TRIGGER reject_cleanup')
                        Path(str(cleanup) + '.fail').unlink(missing_ok=True)
                        wire.begin(connect(), 'a', b'aaaaxxxx')
                        assert not (root / '.delta' / sid).exists()
                        assert not (root / '.delta' / ('.publish-' + sid)).exists()
                    else:
                        wire.send_block(p, sid, 0, b'aaaa')
                        wire.send_block(p, sid, 4, b'bbbb')
                        # Cleanup may fail after durable commit, so confirm by state.
                        try:
                            p.ask(30, dict(session_id=sid), 31)
                        except AssertionError:
                            pass
                        assert connect().ask(28, dict(session_id=sid), 29)['phase'] == 'COMMITTED'
                        wire.stop(server)
                        server = None
                        failed = subprocess.run([wire.SERVER, '--upload', '--root', str(root), '--port', '0'],
                                                env=env, capture_output=True, text=True, timeout=5)
                        assert failed.returncode == 2 and 'committed cleanup failed for session ' + sid in failed.stderr
                        Path(str(cleanup) + '.fail').unlink()
                        server, port = wire.start(root, env)
                        assert not (root / '.delta' / sid).exists()
                        assert (root / 'files' / 'a').read_bytes() == b'aaaabbbb'
                        with sqlite3.connect(root / 'metadata.sqlite3') as db:
                            # Older committed rows survive restart for migration compatibility.
                            assert db.execute('SELECT count(*) FROM manifest_chunks WHERE session=?',
                                              (sid,)).fetchone()[0] == 2
                elif scenario == 'legacy_audit':
                    legacy = wire.Peer(port, delta=False)
                    peers.append(legacy)
                    meta = dict(path='legacy', size=4, mtime_ns=0, chunk_size=4, sha256=wire.digest(b'data'))
                    ready = legacy.ask(3, meta, 4)
                    sid = ready['session_id']
                    wire.send_block(legacy, sid, 0, b'data')
                    legacy.ask(9, dict(session_id=sid), 10)
                    gate.write_text(sid)
                    planner = connect()
                    independent = connect()
                    task = pool.submit(wire.begin, planner, 'legacy', b'data', mode='preview')
                    wait_for(lambda: Path(str(gate) + '.entered').exists(), 'legacy indexing read not entered')
                    independent.sock.settimeout(1)
                    assert wire.begin(independent, 'independent', b'data')['base_version'] is None
                    print('RESOLVED: runtime legacy read permits independent BEGIN', flush=True)
                    independent.close()
                    release.touch()
                    task.result(timeout=3)
                    assert (root / 'files' / 'legacy').read_bytes() == b'data'
                elif scenario == 'recovery_failure':
                    p = connect()
                    ready = wire.begin(p, 'a', b'data')
                    sid = ready['session_id']
                    wire.manifest(p, ready, b'data')
                    wire.plan(p, ready, [[0, 1]])
                    wire.send_block(p, sid, 0, b'data')
                    with sqlite3.connect(root / 'metadata.sqlite3') as db:
                        db.execute("CREATE TRIGGER reject_recovery BEFORE UPDATE OF phase ON delta_sessions "
                                   "WHEN NEW.phase=5 BEGIN SELECT RAISE(ABORT,'test'); END")
                    assert p.ask(30, dict(session_id=sid), 15)['code'] == 'INTERNAL_ERROR'
                    wire.stop(server)
                    server = None
                    for attempt in range(2):
                        failed = subprocess.run([wire.SERVER, '--upload', '--root', str(root), '--port', '0'],
                                                env=env, capture_output=True, text=True, timeout=5)
                        assert failed.returncode == 2 and 'COMMITTING recovery failed for session ' + sid in failed.stderr
                        with sqlite3.connect(root / 'metadata.sqlite3') as db:
                            assert db.execute('SELECT phase FROM delta_sessions WHERE id=?', (sid,)).fetchone()[0] == 4
                    with sqlite3.connect(root / 'metadata.sqlite3') as db:
                        db.execute('DROP TRIGGER reject_recovery')
                    server, port = wire.start(root, env)
                    assert connect().ask(28, dict(session_id=sid), 29)['phase'] == 'COMMITTED'
                    assert (root / 'files' / 'a').read_bytes() == b'data'
                elif scenario == 'orphan_audit':
                    _, old, _ = wire.upload(connect(), 'a', b'aaaabbbb', [[0, 2]])
                    p = connect()
                    ready = prepare(p, 'a', b'aaaaxxxx', mode='upload')
                    sid = ready['session_id']
                    finish(p, ready)
                    wire.plan(p, ready, [[1, 2]])
                    wire.send_block(p, sid, 4, b'xxxx')
                    with sqlite3.connect(root / 'metadata.sqlite3') as db:
                        db.execute("CREATE TRIGGER refuse_prepare BEFORE UPDATE OF phase ON delta_sessions "
                                   "WHEN NEW.phase=4 BEGIN SELECT RAISE(ABORT,'audit'); END")
                    before = len(list((root / '.versions').iterdir()))
                    for n in range(3):
                        assert connect().ask(30, dict(session_id=sid), 15)['code'] == 'INTERNAL_ERROR'
                    after = len(list((root / '.versions').iterdir()))
                    assert after == before + 1
                    owned = json.loads((root / '.preparations' / sid).read_text())
                    assert owned['state'] == 'ready'
                    print('RESOLVED: three failures reuse one durably owned preparation', flush=True)
                    wire.stop(server)
                    server = None
                    server, port = wire.start(root, env)
                    assert not list((root / '.quarantine').glob('version-*'))
                    assert not list((root / '.preparations').iterdir())
                    assert len(list((root / '.versions').iterdir())) == before
                    assert (root / '.versions' / old['version_id'] / 'data').read_bytes() == b'aaaabbbb'
                else:
                    count = 8 if scenario == 'cycles' else 1
                    fd_before = len(list(Path(f'/proc/{server.pid}/fd').iterdir()))
                    for n in range(count):
                        p = connect()
                        content = bytes([65 + n]) * 8
                        ready = wire.begin(p, f'dir/file{n}', content)
                        sid = ready['session_id']
                        wire.manifest(p, ready, content)
                        wire.plan(p, ready, [[0, 2]])
                        wire.send_block(p, sid, 0, content[:4])
                        p.close()
                        resume = connect()
                        assert resume.ask(28, dict(session_id=sid), 29)['missing_bitmap'] == '02'
                        wire.send_block(resume, sid, 4, content[4:])
                        result = resume.ask(30, dict(session_id=sid), 31)
                        assert resume.ask(30, dict(session_id=sid), 31) == result
                        resume.close()
                        assert list((root / '.delta').iterdir()) == []
                    for peer in peers:
                        peer.close()
                    wait_for(lambda: len(list(Path(f'/proc/{server.pid}/fd').iterdir())) <= fd_before + 2,
                             'server FDs did not return to baseline')
                    for repeat in range(3):
                        wire.stop(server)
                        server = None
                        server, port = wire.start(root, env)
                        status = connect().ask(28, dict(session_id=sid), 29)
                        assert status['phase'] == 'COMMITTED' and status['result'] == result
                    with sqlite3.connect(root / 'metadata.sqlite3') as db:
                        assert db.execute('PRAGMA integrity_check').fetchone()[0] == 'ok'
                        assert db.execute('PRAGMA foreign_key_check').fetchall() == []
                        assert db.execute('SELECT count(*) FROM manifest_chunks').fetchone()[0] == 0
                    print('retained committed history objects', count, 'with bounded temporary resources', flush=True)
                print(scenario, 'PASS', flush=True)
            finally:
                release.touch()
                for peer in peers:
                    peer.close()
                if server is not None:
                    wire.stop(server)
                pool.shutdown(wait=True)


if __name__ == '__main__':
    main()
