"""Snapshot protocol on a disposable, real v3 backup copy."""
import os
from pathlib import Path
import sqlite3
import queue
import subprocess
import sys
import tarfile
import tempfile
import threading
import time

import delta_protocol_test as wire
from version_history_test import peer, snapshot

CAP = 'directory_snapshot_v1'
ROOT = 'fixture'
PATH = 'fixture/file.txt'


def connect(port, supported=True):
    import socket
    p = wire.Peer.__new__(wire.Peer)
    p.sock = socket.create_connection(('127.0.0.1', port), timeout=10)
    p.request_id = 0
    ack = p.ask(1, dict(client_name='snapshot-test', versions=[1],
                        capabilities=[CAP] if supported else ['file_list_v1']), 2)
    assert (CAP in ack['capabilities']) == supported
    return p


def create(p, root=ROOT, reply=39):
    return p.ask(37, dict(root=root), reply)


def listing(p, root=ROOT, before=None, limit=32):
    result = p.ask(40, dict(root=root, before=before, limit=limit), 41)
    assert result['root'] == root
    for item in result['snapshots']:
        assert 'root' not in item
        item['root'] = root
    return result


def entries(p, sid, limit=6):
    result = []
    after = ''
    while True:
        page = p.ask(42, dict(snapshot_id=sid, after_path=after, limit=limit), 43)
        assert page['snapshot_id'] == sid and len(page['entries']) <= limit
        result.extend(page['entries'])
        after = page['next_after_path']
        if not after:
            return result
        assert after == result[-1]['path']


def extract(directory):
    with tarfile.open(sys.argv[3]) as archive:
        archive.extractall(directory, filter='data')
    root = Path(directory) / 'storage'
    root.chmod(0o700)
    for item in root.rglob('*'):
        item.chmod(0o700 if item.is_dir() else 0o600)
    return root


def state(db):
    return snapshot(db) | {'history': db.execute('SELECT * FROM version_history ORDER BY sequence').fetchall()}


def main():
    with tempfile.TemporaryDirectory(prefix='chunkharbor-r24-') as directory:
        root = extract(directory)
        db_path = root / 'metadata.sqlite3'
        with sqlite3.connect(db_path) as db:
            assert db.execute('PRAGMA user_version').fetchone() == (3,)
            now = int(time.time())
            db.execute("UPDATE delta_sessions SET created=? WHERE logical_path='fixture/inflight.txt' AND created<?",
                       (now, now - 86400))
            before = state(db)
        migration = (Path(__file__).resolve().parents[2] / 'src/storage/schema_v4.sql').read_text()
        pending = migration.rsplit('COMMIT;', 1)[0]
        with sqlite3.connect(db_path) as db:
            try:
                db.executescript(pending + 'SELECT * FROM injected_missing_table;')
                raise AssertionError('migration fault not injected')
            except sqlite3.OperationalError:
                db.rollback()
            assert state(db) == before and db.execute('PRAGMA user_version').fetchone() == (3,)
        crash = subprocess.run([sys.executable, '-c',
            'import os,sqlite3,sys; db=sqlite3.connect(sys.argv[1]); db.executescript(sys.argv[2]); os._exit(41)',
            str(db_path), pending])
        assert crash.returncode == 41
        with sqlite3.connect(db_path) as db:
            assert state(db) == before and db.execute('PRAGMA user_version').fetchone() == (3,)
            assert not db.execute("SELECT 1 FROM sqlite_master WHERE name='directory_snapshots'").fetchall()
        server, port = wire.start(root)
        try:
            p = connect(port)
            assert listing(p)['snapshots'] == []
            with sqlite3.connect(db_path) as db:
                assert db.execute('PRAGMA user_version').fetchone() == (4,)
                assert state(db) == before
            first = create(p)
            assert first['state'] == 'committed' and first['file_count'] == 2 and first['root'] == ROOT
            assert abs(first['created_at_s'] - time.time()) < 60
            old = entries(p, first['snapshot_id'], limit=1)
            assert [e['path'] for e in old] == ['file.txt', 'legacy.txt']
            assert old[0]['sha256'] == wire.digest(b'R2.2 fixture C')
            assert p.ask(38, dict(snapshot_id=first['snapshot_id']), 39) == first
            upload = wire.Peer(port)
            _, latest, _ = wire.upload(upload, PATH, b'DDDD', [[0, 1]])
            upload.close()
            for path in ['fixture/sub/中文.bin', 'fixture2/excluded']:
                upload = wire.Peer(port)
                wire.upload(upload, path, b'nest', [[0, 1]])
                upload.close()
            second = create(p)
            new = entries(p, second['snapshot_id'])
            assert [e['path'] for e in new] == ['file.txt', 'legacy.txt', 'sub/中文.bin']
            assert new[0]['version_id'] == latest['version_id']
            assert entries(p, first['snapshot_id']) == old
            newest = listing(p, limit=1)
            assert newest['snapshots'] == [second]
            earlier = listing(p, before=newest['next_before'], limit=1)
            assert earlier['snapshots'] == [first] and earlier['next_before'] is None
            empty = create(p, 'missing/empty')
            assert empty['file_count'] == 0 and entries(p, empty['snapshot_id']) == []
            single = create(p, 'fixture/sub')
            assert single['file_count'] == 1 and entries(p, single['snapshot_id'])[0]['path'] == '中文.bin'
            old_peer = connect(port, False)
            assert old_peer.ask(11, dict(after_path='', limit=32), 12)['files']
            assert create(old_peer, reply=15)['code'] == 'UNSUPPORTED_OPERATION'
            old_peer.close()
            for kind, payload, code in [
                    (37, dict(root='../unsafe'), 'MALFORMED_MESSAGE'),
                    (37, dict(root='x' * 4097), 'MALFORMED_MESSAGE'),
                    (38, dict(snapshot_id='ff' * 16), 'SNAPSHOT_NOT_FOUND'),
                    (38, dict(snapshot_id='x'), 'MALFORMED_MESSAGE'),
                    (40, dict(root=ROOT, before=0, limit=1), 'MALFORMED_MESSAGE'),
                    (40, dict(root=ROOT, before=None, limit=65), 'MALFORMED_MESSAGE'),
                    (42, dict(snapshot_id=first['snapshot_id'], after_path='../x', limit=6), 'MALFORMED_MESSAGE'),
                    (42, dict(snapshot_id=first['snapshot_id'], after_path='', limit=7), 'MALFORMED_MESSAGE')]:
                bad = connect(port)
                assert bad.ask(kind, payload, 15)['code'] == code
                bad.close()
            # Upload a client directory twice. A missing source file and an empty
            # subdirectory do not translate into remote deletes or directory rows.
            source = Path(directory) / 'source'
            source.mkdir()
            (source / 'empty').mkdir()
            (source / 'stay').write_bytes(b'stays')
            (source / 'gone').write_bytes(b'goes')
            command = [sys.argv[2], 'backup', '--directory', str(source), '--prefix', 'deletion',
                       '--state-dir', str(Path(directory) / 'states'), '--port', str(port)]
            subprocess.run(command, capture_output=True, check=True)
            (source / 'gone').unlink()
            subprocess.run(command, capture_output=True, check=True)
            removed = create(p, 'deletion')
            assert [e['path'] for e in entries(p, removed['snapshot_id'])] == ['gone', 'stay']

            legacy_source = Path(directory) / 'legacy-source'
            legacy_source.write_bytes(b'unnormalized legacy')
            subprocess.run([sys.argv[2], 'upload', '--port', str(port), '--file', str(legacy_source),
                            '--path', 'fresh-legacy/item'], check=True, capture_output=True)
            fresh = create(p, 'fresh-legacy')
            assert entries(p, fresh['snapshot_id'])[0]['sha256'] == wire.digest(b'unnormalized legacy')
            with sqlite3.connect(db_path) as db:
                assert db.execute('SELECT version_ref,legacy_ref FROM snapshot_entries WHERE snapshot_id=?',
                                  (fresh['snapshot_id'],)).fetchone()[0] is None
                db.execute("CREATE TRIGGER fail_snapshot BEFORE INSERT ON snapshot_entries "
                           "WHEN EXISTS(SELECT 1 FROM snapshot_entries WHERE snapshot_id=NEW.snapshot_id) "
                           "BEGIN SELECT RAISE(ABORT,'injected failure after first entry'); END")
            failed = connect(port)
            assert create(failed, reply=15)['code'] == 'INTERNAL_ERROR'
            failed.close()
            assert listing(p)['snapshots'] == [second, first]
            with sqlite3.connect(db_path) as db:
                assert db.execute("SELECT count(*) FROM directory_snapshots WHERE state='creating'").fetchone() == (0,)
                db.execute('DROP TRIGGER fail_snapshot')
                db.execute('INSERT INTO current_files(path,version_id) VALUES(?,?)',
                           ('bad/mismatch', old[0]['version_id']))
            broken = connect(port)
            assert create(broken, 'bad', reply=15)['code'] == 'INTERNAL_ERROR'
            broken.close()
            assert listing(p, 'bad')['snapshots'] == []
            with sqlite3.connect(db_path) as db:
                db.execute("DELETE FROM current_files WHERE path='bad/mismatch'")
                db.execute("INSERT INTO directory_snapshots(id,root,created_at_s,state,file_count) "
                           "VALUES(?, 'uncommitted', 0, 'creating', 0)", ('ee' * 16,))
            hidden = connect(port)
            assert hidden.ask(38, dict(snapshot_id='ee' * 16), 15)['code'] == 'SNAPSHOT_NOT_FOUND'
            hidden.close()
            hidden = connect(port)
            assert hidden.ask(42, dict(snapshot_id='ee' * 16, after_path='', limit=6), 15)['code'] == 'SNAPSHOT_NOT_FOUND'
            hidden.close()
            assert listing(p, 'uncommitted')['snapshots'] == []
            with sqlite3.connect(db_path) as db:
                db.execute("DELETE FROM directory_snapshots WHERE state='creating'")
                db.commit()
                assert not db.execute('PRAGMA foreign_key_check').fetchall()
                db.execute('PRAGMA foreign_keys=ON')
                for table, key in [('versions', 'id'), ('version_history', 'version_id')]:
                    try:
                        db.execute(f'DELETE FROM {table} WHERE {key}=?', (old[0]['version_id'],))
                        raise AssertionError('referenced version was deletable')
                    except sqlite3.IntegrityError:
                        pass
            for index in range(70):
                upload = wire.Peer(port)
                wire.upload(upload, f'large/{index:03}.txt', b'page', [[0, 1]])
                upload.close()
            large = create(p, 'large')
            assert large['file_count'] == 70
            assert [item['path'] for item in entries(p, large['snapshot_id'])] == [
                f'{index:03}.txt' for index in range(70)]
            p.close()
        finally:
            wire.stop(server)
        server, port = wire.start(root)
        try:
            p = connect(port)
            assert entries(p, first['snapshot_id']) == old and entries(p, second['snapshot_id']) == new
            # Synthetic published metadata exercises the count guard without
            # creating 10,001 files. The fixture copy is discarded after this check.
            rows = [(f'{0xff000000 + i:032x}', f'quota/{i:05}') for i in range(10001)]
            with sqlite3.connect(db_path) as db:
                db.executemany("INSERT INTO versions VALUES(?,?,0,1,?)",
                               [(vid, path, wire.digest(b'')) for vid, path in rows])
                db.executemany("INSERT INTO current_files(path,version_id) VALUES(?,?)",
                               [(path, vid) for vid, path in rows])
            assert create(p, 'quota', reply=15)['code'] == 'QUOTA_EXCEEDED'
            p.close()
            check = connect(port)
            assert listing(check, 'quota')['snapshots'] == []
            check.close()
            with sqlite3.connect(db_path) as db:
                assert db.execute("SELECT count(*) FROM directory_snapshots WHERE root='quota'").fetchone() == (0,)
        finally:
            wire.stop(server)
    print('v3 migration, prefix scope, committed snapshots, pagination and restart passed')


def fault_test(hook):
    for crash in (False, True):
        with tempfile.TemporaryDirectory(prefix='chunkharbor-r24-txn-') as directory:
            base = Path(directory)
            root = extract(base)
            marker, release = base / 'paused', base / 'release'
            env = dict(os.environ, LD_PRELOAD=hook, CH_M4_TEST_CRASH_STAGE='snapshot_before_commit',
                       CH_M4_TEST_CRASH_MARKER=str(marker))
            if not crash:
                env['CH_SNAPSHOT_TEST_RELEASE'] = str(release)
            server, port = wire.start(root, env)
            workers = []
            outputs = queue.Queue()
            peers = []
            def run(action):
                def job():
                    try: outputs.put(action())
                    except Exception as exc: outputs.put(exc)
                worker = threading.Thread(target=job, daemon=True)
                workers.append(worker)
                worker.start()
            try:
                reader = connect(port)
                peers.append(reader)
                upload = wire.Peer(port)
                peers.append(upload)
                ready = wire.begin(upload, PATH, b'DDDD')
                wire.manifest(upload, ready, b'DDDD')
                wire.plan(upload, ready, [[0, 1]])
                wire.send_block(upload, ready['session_id'], 0, b'DDDD')
                run(lambda: create(reader))
                deadline = time.monotonic() + 8
                while not marker.exists():
                    assert server.poll() is None and time.monotonic() < deadline, 'missing precommit marker'
                    time.sleep(.01)
                with sqlite3.connect(root / 'metadata.sqlite3', timeout=0) as db:
                    assert db.execute('SELECT count(*) FROM directory_snapshots').fetchone() == (0,)
                    assert db.execute('SELECT count(*) FROM snapshot_entries').fetchone() == (0,)
                    try:
                        db.execute('BEGIN IMMEDIATE')
                        raise AssertionError('second writer acquired the snapshot write transaction')
                    except sqlite3.OperationalError as exc:
                        assert 'locked' in str(exc), exc
                if crash:
                    wire.stop(server, crash=True)
                    assert isinstance(outputs.get(timeout=10), Exception)
                    server, port = wire.start(root)
                    after = connect(port)
                    peers.append(after)
                    assert listing(after)['snapshots'] == []
                    assert create(after)['file_count'] == 2
                else:
                    attempted = threading.Event()
                    def commit():
                        attempted.set()
                        return upload.ask(30, dict(session_id=ready['session_id']), 31)
                    run(commit)
                    assert attempted.wait(5)
                    time.sleep(.05)
                    assert outputs.empty(), 'snapshot and upload completed before barrier released'
                    release.touch()
                    replies = [outputs.get(timeout=12), outputs.get(timeout=12)]
                    assert all(isinstance(reply, dict) for reply in replies), replies
                    captured = next(reply for reply in replies if 'snapshot_id' in reply)
                    assert entries(reader, captured['snapshot_id'])[0]['sha256'] == wire.digest(b'R2.2 fixture C')
                    assert entries(reader, create(reader)['snapshot_id'])[0]['sha256'] == wire.digest(b'DDDD')
            finally:
                release.touch(exist_ok=True)
                if server.poll() is None:
                    wire.stop(server, crash=crash and marker.exists() and len(peers) == 2)
                for worker in workers:
                    worker.join(12)
                for p in peers:
                    p.close()
    print('concurrent backup commit serialization and SIGKILL rollback passed')


def remote_smoke(port):
    p = connect(port)
    try:
        assert listing(p)['snapshots'] == []
        captured = create(p)
        assert captured['state'] == 'committed' and captured['file_count'] == 2
        assert p.ask(38, dict(snapshot_id=captured['snapshot_id']), 39) == captured
        assert listing(p)['snapshots'] == [captured]
        rows = entries(p, captured['snapshot_id'], limit=1)
        assert [(e['path'], e['sha256']) for e in rows] == [
            ('file.txt', wire.digest(b'R2.2 fixture C')), ('legacy.txt', wire.digest(b'legacy fixture'))]
        print('Snapshot', captured['snapshot_id'], 'committed with', len(rows), 'verified entries')
    finally:
        p.close()


if __name__ == '__main__':
    if len(sys.argv) > 5 and sys.argv[4] == '--remote':
        remote_smoke(int(sys.argv[5]))
    elif len(sys.argv) > 4:
        fault_test(sys.argv[4])
    else:
        main()
