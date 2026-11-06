"""A.5.2 immutable legacy indexing, barriers and persisted-state assertions."""
import concurrent.futures
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import time

import delta_protocol_test as wire
from delta_plan_lock_test import wait_for


def old_upload(port, path, data=b'aaaabbbb', chunk=4):
    peer = wire.Peer(port, delta=False)
    try:
        ready = peer.ask(3, dict(path=path, size=len(data), mtime_ns=0,
                                 chunk_size=chunk, sha256=wire.digest(data)), 4)
        for offset in range(0, len(data), chunk):
            wire.send_block(peer, ready['session_id'], offset, data[offset:offset + chunk])
        peer.ask(9, dict(session_id=ready['session_id']), 10)
        return ready['session_id']
    finally:
        peer.close()


def indexed(root, sid, data, chunk=4):
    with sqlite3.connect(root / 'metadata.sqlite3') as db:
        assert db.execute('SELECT version_id FROM legacy_versions WHERE session_id=?',
                          (sid,)).fetchone() == (sid,)
        assert db.execute('SELECT idx,sha256,length FROM version_chunks WHERE version_id=? ORDER BY idx',
                          (sid,)).fetchall() == [
            (i // chunk, wire.digest(data[i:i + chunk]), len(data[i:i + chunk]))
            for i in range(0, len(data), chunk)]
        assert db.execute('PRAGMA integrity_check').fetchone() == ('ok',)
    assert (root / '.versions' / sid / 'data').read_bytes() == data


def absent(root, sid):
    with sqlite3.connect(root / 'metadata.sqlite3') as db:
        assert db.execute('SELECT 1 FROM legacy_versions WHERE session_id=?', (sid,)).fetchone() is None
        assert db.execute('SELECT 1 FROM current_files WHERE version_id=?', (sid,)).fetchone() is None
        assert db.execute('SELECT state FROM transfers WHERE session_id=?', (sid,)).fetchone() == ('COMMITTED',)


def history(root, sid):
    with sqlite3.connect(root / 'metadata.sqlite3') as db:
        return db.execute('SELECT * FROM version_history WHERE version_id=?', (sid,)).fetchall()


def main():
    # --baseline retains the old blocking experiment; normal mode must fail on the old binary.
    baseline = '--baseline' in sys.argv
    scenarios = ('progress',) if baseline else (
        'normal', 'empty', 'partial_block', 'max_file', 'max_blocks', 'progress',
        'duplicate', 'same_path_update', 'rename_current', 'rename_object', 'missing',
        'read_failure', 'checksum_failure', 'mutate_during_read', 'sql_rollback',
        'disconnect', 'shutdown', 'restart', 'lease_operations', 'owner_cleanup',
        'exact_conflict', 'symlink', 'cycles')
    for scenario in scenarios:
        with tempfile.TemporaryDirectory(prefix='chunkharbor-legacy-lock-') as tmp:
            root = Path(tmp) / 'backup'
            root.mkdir(mode=0o700)
            gate = Path(tmp) / 'gate'
            entered, release = Path(str(gate) + '.entered'), Path(str(gate) + '.release')
            env = dict(os.environ, LD_PRELOAD=sys.argv[2], CH_PLAN_TEST_GATE=str(gate))
            server, port = wire.start(root, env)
            peers = []
            pool = concurrent.futures.ThreadPoolExecutor(max_workers=2)
            def connect():
                p = wire.Peer(port)
                peers.append(p)
                return p
            try:
                fd_initial = len(list(Path('/proc/' + str(server.pid) + '/fd').iterdir()))
                if scenario in ('normal', 'empty', 'partial_block', 'max_file', 'max_blocks', 'cycles'):
                    data = b'aaaabbbb'
                    if scenario == 'empty':
                        data = b''
                    elif scenario == 'partial_block':
                        data = b'abcde'
                    elif scenario == 'max_file':
                        data = b'abcd' * (16 * 1024 * 1024)
                    elif scenario == 'max_blocks':
                        data = b'a' * 65536
                    chunk = 262144 if scenario == 'max_file' else 4
                    if scenario == 'max_blocks':
                        chunk = 65536
                    for cycle in range(10 if scenario == 'cycles' else 1):
                        name = 'legacy' + str(cycle)
                        sid = old_upload(port, name, data, chunk)
                        if scenario == 'max_blocks':
                            # A durable, valid terminal legacy geometry at the existing block cap.
                            with sqlite3.connect(root / 'metadata.sqlite3') as db:
                                db.execute('UPDATE transfers SET chunk_size=1 WHERE session_id=?', (sid,))
                                db.execute('DELETE FROM transfer_chunks WHERE session_id=?', (sid,))
                                db.executemany('INSERT INTO transfer_chunks VALUES(?,?,?,1)',
                                               ((sid, i, wire.digest(b'a')) for i in range(len(data))))
                            chunk = 1
                        before = history(root, sid)
                        p = connect()
                        ready = wire.begin(p, name, data, chunk_size=chunk, mode='preview')
                        assert ready['base_version'] == sid
                        wire.manifest(p, ready, data, chunk)
                        _, done = wire.plan(p, ready, [])
                        assert done['missing_bytes'] == 0
                        indexed(root, sid, data, chunk)
                        assert history(root, sid) == before
                        p.close()
                    wait_for(lambda: len(list(Path('/proc/' + str(server.pid) + '/fd').iterdir())) <= fd_initial,
                             'index FD or connection leaked')
                    print(scenario, 'PASS', flush=True)
                    continue
                # Prepare independent delta work before the legacy record exists.
                worker = None
                if scenario in ('lease_operations', 'owner_cleanup'):
                    p = connect()
                    wire.upload(p, 'other', b'ccccdddd', [[0, 2]])
                    worker = connect()
                    wr = wire.begin(worker, 'other', b'zzzzdddd')
                    if scenario == 'owner_cleanup':
                        wire.manifest(worker, wr, b'zzzzdddd')
                        wire.plan(worker, wr, [[0, 1]])
                        wire.send_block(worker, wr['session_id'], 0, b'zzzz')
                        with sqlite3.connect(root / 'metadata.sqlite3') as db:
                            db.execute("CREATE TRIGGER refuse_owner BEFORE UPDATE OF phase ON delta_sessions "
                                       "WHEN NEW.phase=4 BEGIN SELECT RAISE(ABORT,'test'); END")
                        assert worker.ask(30, dict(session_id=wr['session_id']), 15)['code'] == 'INTERNAL_ERROR'
                        owned = json.loads((root / '.preparations' / wr['session_id']).read_text())
                        with sqlite3.connect(root / 'metadata.sqlite3') as db:
                            db.execute('DROP TRIGGER refuse_owner')
                            db.execute('UPDATE delta_sessions SET created=0 WHERE id=?', (wr['session_id'],))
                sid = old_upload(port, 'legacy')
                before = history(root, sid)
                if scenario == 'same_path_update':
                    # Reindex a real committed version while an already prepared update publishes.
                    wire.begin(connect(), 'legacy', b'aaaabbbb', mode='preview')
                    updater = connect()
                    update = wire.begin(updater, 'legacy', b'zzzzbbbb')
                    wire.manifest(updater, update, b'zzzzbbbb')
                    wire.plan(updater, update, [[0, 1]])
                    wire.send_block(updater, update['session_id'], 0, b'zzzz')
                    with sqlite3.connect(root / 'metadata.sqlite3') as db:
                        db.execute('DELETE FROM legacy_versions WHERE session_id=?', (sid,))
                        db.execute('DELETE FROM version_chunks WHERE version_id=?', (sid,))
                obj = root / '.versions' / sid / 'data'
                if scenario in ('missing', 'checksum_failure', 'symlink'):
                    saved = obj.with_name('saved')
                    if scenario == 'checksum_failure':
                        obj.write_bytes(b'xxxxxxxx')
                    else:
                        obj.rename(saved)
                        if scenario == 'symlink':
                            obj.symlink_to(saved)
                    p = connect()
                    error = p.ask(24, dict(metadata=dict(path='legacy', size=8, mtime_ns=0,
                                      chunk_size=4, sha256=wire.digest(b'aaaabbbb')),
                                      mode='preview', base_version='auto'), 15)['code']
                    assert error == ('INVALID_PATH' if scenario == 'symlink' else 'INTERNAL_ERROR'), error
                    absent(root, sid)
                    if scenario == 'checksum_failure':
                        obj.write_bytes(b'aaaabbbb')
                    else:
                        if scenario == 'symlink':
                            obj.unlink()
                        saved.rename(obj)
                    assert wire.begin(connect(), 'legacy', b'aaaabbbb', mode='preview')['base_version'] == sid
                    indexed(root, sid, b'aaaabbbb')
                    print(scenario, 'PASS', flush=True)
                    continue
                if scenario == 'sql_rollback':
                    with sqlite3.connect(root / 'metadata.sqlite3') as db:
                        db.execute("CREATE TRIGGER refuse_index BEFORE INSERT ON legacy_versions "
                                   "BEGIN SELECT RAISE(ABORT,'test'); END")
                gate.write_text(sid)
                planner, other = connect(), connect()
                expected_error = scenario in ('same_path_update', 'rename_object', 'read_failure',
                                              'mutate_during_read', 'sql_rollback', 'exact_conflict')
                meta = dict(path='legacy', size=8, mtime_ns=0, chunk_size=4, sha256=wire.digest(b'aaaabbbb'))
                task = pool.submit(planner.ask, 24, dict(metadata=meta, mode='preview',
                                   base_version='f' * 32 if scenario == 'exact_conflict' else 'auto'),
                                   15 if expected_error else 25)
                wait_for(entered.exists, 'legacy pread not entered')
                # The barrier holds a disk worker but no SQLite write transaction.
                with sqlite3.connect(root / 'metadata.sqlite3', timeout=1) as db:
                    db.execute('BEGIN IMMEDIATE')
                    db.rollback()
                other.sock.settimeout(0.3 if baseline else 1)
                started = time.monotonic()
                try:
                    answer = wire.begin(other, 'independent', b'data')
                    assert not baseline, 'old behavior did not block'
                    assert answer['base_version'] is None
                except TimeoutError:
                    assert baseline, 'legacy body read still blocks independent BEGIN'
                    print('old worker waits:', [(p.name, (p / 'wchan').read_text())
                          for p in Path('/proc/' + str(server.pid) + '/task').iterdir()], flush=True)
                elapsed = time.monotonic() - started
                assert baseline or elapsed < 1, ('independent BEGIN exceeded deadline', elapsed)
                print('baseline' if baseline else 'unlocked', 'independent BEGIN seconds',
                      round(elapsed, 4), flush=True)
                if scenario == 'duplicate':
                    duplicate = connect()
                    duplicate.sock.settimeout(1)
                    assert duplicate.ask(24, dict(metadata=meta, mode='preview', base_version='auto'), 15)['code'] == 'BUSY'
                    assert len(entered.read_bytes()) == 1
                elif scenario == 'same_path_update':
                    published = updater.ask(30, dict(session_id=update['session_id']), 31)
                    assert published['committed']
                elif scenario == 'rename_current':
                    (root / 'files' / 'legacy').rename(root / 'files' / 'renamed')
                    replacement = root / 'files' / 'legacy'
                    replacement.write_bytes(b'zzzzbbbb')
                    replacement.chmod(0o600)
                elif scenario == 'rename_object':
                    obj.rename(obj.with_name('saved'))
                    assert any(os.readlink(fd).endswith('/' + sid + '/saved')
                               for fd in Path('/proc/' + str(server.pid) + '/fd').iterdir())
                elif scenario == 'read_failure':
                    Path(str(gate) + '.fail').touch()
                elif scenario == 'mutate_during_read':
                    obj.write_bytes(b'xxxxxxxx')
                elif scenario == 'disconnect':
                    planner.close()
                elif scenario == 'shutdown':
                    server.terminate()
                elif scenario == 'restart':
                    wire.stop(server, crash=True)
                    server = None
                elif scenario == 'lease_operations':
                    wire.manifest(worker, wr, b'zzzzdddd')
                    wire.plan(worker, wr, [[0, 1]])
                    wire.send_block(worker, wr['session_id'], 0, b'zzzz')
                    assert connect().ask(28, dict(session_id=wr['session_id']), 29)['phase'] == 'TRANSFERRING'
                    assert worker.ask(30, dict(session_id=wr['session_id']), 31)['committed']
                elif scenario == 'owner_cleanup':
                    assert not (root / '.preparations' / wr['session_id']).exists()
                    assert not (root / '.versions' / owned['version']).exists()
                    assert obj.exists()
                release.touch()
                if scenario in ('disconnect', 'shutdown', 'restart'):
                    try:
                        task.result(timeout=4)
                    except (OSError, AssertionError):
                        pass
                    if scenario == 'shutdown':
                        server.communicate(timeout=4)
                        assert server.returncode == 0
                        server = None
                    if scenario in ('shutdown', 'restart'):
                        gate.unlink()
                        server, port = wire.start(root, env)
                    wait_for(lambda: bool(history(root, sid)), 'legacy history lost')
                    assert wire.begin(connect(), 'legacy', b'aaaabbbb', mode='preview')['base_version'] == sid
                elif expected_error:
                    error = task.result(timeout=4)['code']
                    assert error == ('INTERNAL_ERROR' if scenario in ('read_failure', 'mutate_during_read', 'sql_rollback') else 'CONFLICT'), error
                    if scenario == 'same_path_update':
                        with sqlite3.connect(root / 'metadata.sqlite3') as db:
                            assert db.execute("SELECT version_id FROM current_files WHERE path='legacy'").fetchone() == (published['version_id'],)
                        assert (root / 'files' / 'legacy').read_bytes() == b'zzzzbbbb'
                    elif scenario != 'exact_conflict':
                        absent(root, sid)
                        with sqlite3.connect(root / 'metadata.sqlite3') as db:
                            assert db.execute('SELECT 1 FROM versions WHERE id=?', (sid,)).fetchone() is None
                    if scenario == 'rename_object':
                        obj.with_name('saved').rename(obj)
                    elif scenario == 'mutate_during_read':
                        obj.write_bytes(b'aaaabbbb')
                    elif scenario == 'read_failure':
                        Path(str(gate) + '.fail').unlink()
                    elif scenario == 'sql_rollback':
                        with sqlite3.connect(root / 'metadata.sqlite3') as db:
                            db.execute('DROP TRIGGER refuse_index')
                    retry = wire.begin(connect(), 'legacy', b'aaaabbbb', mode='preview')
                    assert retry['base_version'] == (published['version_id'] if scenario == 'same_path_update' else sid)
                else:
                    assert task.result(timeout=4)['base_version'] == sid
                indexed(root, sid, b'aaaabbbb')
                assert history(root, sid) == before
                print(scenario, 'PASS', flush=True)
            finally:
                release.touch()
                pool.shutdown(wait=True)
                for p in peers:
                    p.close()
                if server:
                    wire.stop(server)


if __name__ == '__main__':
    main()
