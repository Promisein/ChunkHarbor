"""Exercise the v2 server-generated backup on a disposable copy."""
import hashlib
import time
import pathlib
import socket
import sqlite3
import subprocess
import sys
import tarfile
import tempfile

import delta_protocol_test as wire

SERVER, CLI, FIXTURE = sys.argv[1:4]
CAP = 'version_history_v1'


def peer(port, caps):
    p = wire.Peer.__new__(wire.Peer)
    p.sock = socket.create_connection(('127.0.0.1', port), timeout=10)
    p.request_id = 0
    ack = p.ask(1, dict(client_name='history-test', versions=[1], capabilities=caps), 2)
    assert (CAP in ack['capabilities']) == (CAP in caps)
    return p


def page(p, path='fixture/file.txt', before=None, limit=32):
    return p.ask(34, dict(path=path, before=before, limit=limit), 35)


def snapshot(db):
    return {table: db.execute(f'SELECT * FROM {table} ORDER BY 1,2').fetchall()
            for table in ('versions', 'version_chunks', 'current_files', 'delta_sessions',
                          'manifest_chunks', 'transfers', 'transfer_chunks')}


def main():
    with tempfile.TemporaryDirectory(prefix='chunkharbor-r22-') as directory:
        base = pathlib.Path(directory)
        with tarfile.open(FIXTURE) as archive:
            archive.extractall(base, filter='data')
        root = base / 'storage'
        root.chmod(0o700)
        for item in root.rglob('*'):
            item.chmod(0o700 if item.is_dir() else 0o600)
        db_path = root / 'metadata.sqlite3'
        with sqlite3.connect(db_path) as db:
            assert db.execute('PRAGMA user_version').fetchone() == (2,)
            # Keep the fixture usable after the real 30-day lease expires.
            # Only the disposable copy's active-session lease is refreshed.
            now = int(time.time())
            db.execute("UPDATE delta_sessions SET created=? WHERE logical_path=? AND created<?",
                       (now, 'fixture/inflight.txt', now - 86400))
            old = snapshot(db)
            old_ids = [r[0] for r in db.execute(
                "SELECT id FROM versions WHERE path='fixture/file.txt' ORDER BY rowid DESC")]
            assert len(old_ids) == 3
        migration = (pathlib.Path(__file__).resolve().parents[2] / 'src/storage/schema_v3.sql').read_text()
        before_commit = migration.rsplit('COMMIT;', 1)[0]
        with sqlite3.connect(db_path) as db:
            try:
                db.executescript(before_commit + 'SELECT * FROM injected_missing_table;')
                raise AssertionError('migration error was not injected')
            except sqlite3.OperationalError:
                db.rollback()
            assert snapshot(db) == old
            assert db.execute('PRAGMA user_version').fetchone() == (2,)
        # Abrupt process exit with all DDL/backfill executed and no COMMIT.
        interrupted = subprocess.run([sys.executable, '-c',
            'import os,sqlite3,sys; db=sqlite3.connect(sys.argv[1]); '
            'db.executescript(sys.argv[2]); os._exit(41)', str(db_path), before_commit])
        assert interrupted.returncode == 41
        with sqlite3.connect(db_path) as db:
            assert snapshot(db) == old
            assert db.execute('PRAGMA user_version').fetchone() == (2,)
            assert not db.execute("SELECT 1 FROM sqlite_master WHERE name='version_history'").fetchall()
        server, port = wire.start(root)
        try:
            p = peer(port, [CAP])
            first = page(p, limit=1)
            assert first['versions'][0]['version_id'] == old_ids[0]
            assert first['versions'][0]['current'] is True
            assert first['versions'][0]['committed_at_s'] is None
            assert page(p, 'no-file')['versions'] == []
            assert len(page(p, 'fixture/legacy.txt')['versions']) == 1
            for path, expected in [('fixture/file.txt', b'R2.2 fixture C'),
                                   ('fixture/legacy.txt', b'legacy fixture')]:
                restored = base / pathlib.Path(path).name
                subprocess.run([CLI, 'download', '--port', str(port), '--path', path,
                                '--output', str(restored)], check=True, capture_output=True)
                assert restored.read_bytes() == expected
            with sqlite3.connect(db_path) as db:
                new = snapshot(db)
                # Startup normalizes the existing legacy upload. Every old row survives.
                for table, rows in old.items():
                    assert all(row in new[table] for row in rows), table
                assert db.execute('PRAGMA user_version').fetchone() == (4,)
                assert not db.execute('PRAGMA foreign_key_check').fetchall()
                plan = db.execute("EXPLAIN QUERY PLAN SELECT h.version_id,COALESCE(v.size,t.total_size),"
                                  "COALESCE(v.sha256,t.expected_sha256),h.sequence,h.committed_at_s,"
                                  "CASE WHEN c.version_id IS NOT NULL THEN c.version_id=h.version_id "
                                  "ELSE COALESCE(t.state='COMMITTED',0) END FROM version_history h "
                                  "LEFT JOIN versions v ON v.id=h.version_id "
                                  "LEFT JOIN transfers t ON t.session_id=h.version_id "
                                  "LEFT JOIN current_files c ON c.path=h.path WHERE h.path=? AND h.sequence<=? "
                                  "ORDER BY h.sequence DESC LIMIT ?",
                                  ('fixture/file.txt', 10, 2)).fetchall()
                assert any('history_by_path_sequence' in row[-1] for row in plan), plan
                assert not any('TEMP B-TREE' in row[-1] for row in plan), plan
                print('EXPLAIN:', [row[-1] for row in plan])
            pending_id = next(row[0] for row in old['delta_sessions'] if row[2] == 'fixture/inflight.txt')
            assert page(p, 'fixture/inflight.txt')['versions'] == []
            resumed = wire.Peer(port)
            status = resumed.ask(28, {'session_id': pending_id}, 29)
            assert status['missing_bitmap'] == '02', status
            wire.send_block(resumed, pending_id, 4, b'EFGH')
            resumed.ask(30, {'session_id': pending_id}, 31)
            resumed.close()
            assert page(p, 'fixture/inflight.txt')['versions'][0]['current']
            up = wire.Peer(port)
            wire.upload(up, 'fixture/file.txt', b'DDDD', [[0, 1]])
            up.close()
            rest = page(p, before=first['next_before'], limit=2)
            assert [r['version_id'] for r in rest['versions']] == old_ids[1:]
            assert rest['next_before'] is None and not any(r['current'] for r in rest['versions'])
            head = page(p)['versions']
            assert len(head) == 4 and head[0]['current']
            assert abs(head[0]['committed_at_s'] - time.time()) < 60
            assert head[0]['sha256'] == hashlib.sha256(b'DDDD').hexdigest()
            same = wire.Peer(port)
            wire.upload(same, 'fixture/file.txt', b'DDDD', [])
            same.close()
            assert page(p)['versions'] == head
            legacy_source = base / 'legacy-source'
            legacy_source.write_bytes(b'new legacy content')
            subprocess.run([CLI, 'upload', '--port', str(port), '--file', str(legacy_source),
                            '--path', 'new-legacy'], check=True, capture_output=True)
            legacy = page(p, 'new-legacy')['versions']
            assert len(legacy) == 1 and legacy[0]['current'] and legacy[0]['committed_at_s']
            old_peer = peer(port, ['file_list_v1', 'download_v1'])
            assert old_peer.ask(11, dict(after_path='', limit=32), 12)['files']
            assert old_peer.ask(13, dict(path='fixture/file.txt', version_id=old_ids[0]), 15)['code'] == 'VERSION_CONFLICT'
            old_peer.close()
            denied = peer(port, ['handshake'])
            assert denied.ask(34, dict(path='x', before=None, limit=1), 15)['code'] == 'UNSUPPORTED_OPERATION'
            denied.close()
            for payload in [dict(path='x', before=None, limit=65), dict(path='../x', before=None, limit=1),
                            dict(path='x' * 4097, before=None, limit=1), dict(path='x', before=0, limit=1),
                            dict(path='x', before=1.5, limit=1)]:
                bad = peer(port, [CAP])
                assert bad.ask(34, payload, 15)['code'] == 'MALFORMED_MESSAGE'
                bad.close()
            for index in range(65):
                up = wire.Peer(port)
                wire.upload(up, 'many', index.to_bytes(4, 'big'), [[0, 1]])
                up.close()
            large = page(p, 'many', limit=64)
            assert len(large['versions']) == 64
            end = page(p, 'many', before=large['next_before'], limit=64)
            assert len(end['versions']) == 1 and end['next_before'] is None
            p.close()
        finally:
            server.terminate()
            server.wait(timeout=10)
        server, port = wire.start(root)
        try:
            p = peer(port, [CAP])
            assert page(p)['versions'] == head
            p.close()
        finally:
            server.terminate()
            server.wait(timeout=10)
    print('v2 fixture migration, ordered paging, live commit, compatibility and reopen passed')


if __name__ == '__main__':
    main()
