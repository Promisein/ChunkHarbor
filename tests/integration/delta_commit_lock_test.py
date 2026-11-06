"""Commit barriers and persisted-state assertions on private server roots."""
import concurrent.futures
import os
from pathlib import Path
import sqlite3
import sys
import tempfile
import time

import delta_protocol_test as wire
from delta_plan_lock_test import wait_for


class DropCommitReply:
    """Consume the real success frame before exposing EOF to the client."""
    def __init__(self, sock):
        self.sock = sock
        self.dropped = False

    def sendall(self, data):
        self.sock.sendall(data)

    def recv(self, length):
        header = wire.HEADER.unpack(wire.exact(self.sock, 32))
        assert header[2] == 31
        assert wire.json.loads(wire.exact(self.sock, header[5]))['committed']
        self.dropped = True
        self.sock.close()
        return b''

    def close(self):
        self.sock.close()


def prepare(peer, path, content=b'zzzzbbbb'):
    ready = wire.begin(peer, path, content)
    wire.manifest(peer, ready, content)
    # Initial old content is aaaabbbb, so only block zero changes.
    wire.plan(peer, ready, [[0, 1]])
    wire.send_block(peer, ready['session_id'], 0, content[:4])
    return ready


def rows(root, sid):
    with sqlite3.connect(root / 'metadata.sqlite3') as db:
        return (db.execute('SELECT phase,version_id FROM delta_sessions WHERE id=?', (sid,)).fetchone(),
                db.execute("SELECT version_id FROM current_files WHERE path='a'").fetchone()[0],
                db.execute('PRAGMA integrity_check').fetchone()[0],
                db.execute('PRAGMA foreign_key_check').fetchall())


def main():
    scenarios = ('hash', 'baseline', 'object_copy', 'publish_copy', 'object_sync', 'publish_sync',
                 'duplicate', 'query', 'write', 'disconnect', 'parallel', 'path_conflict', 'expiry',
                 'copy_error', 'sync_error', 'prepare_sql', 'publish_sql', 'runtime_retry', 'lost_response',
                 'crash_prepared', 'crash_published', 'pointer_race', 'target_race', 'checksum_error',
                 'publish_pointer_race', 'publish_target_race')
    for scenario in scenarios:
        with tempfile.TemporaryDirectory(prefix='chunkharbor-commit-') as tmp:
            root = Path(tmp) / 'backup'
            root.mkdir(mode=0o700)
            gate = Path(tmp) / 'gate'
            release = Path(str(gate) + '.release')
            entered = Path(str(gate) + '.entered')
            env = dict(os.environ, LD_PRELOAD=sys.argv[2], CH_COMMIT_TEST_GATE=str(gate))
            server, port = wire.start(root, env)
            peers = []
            pool = concurrent.futures.ThreadPoolExecutor(max_workers=2)
            def connect():
                peer = wire.Peer(port)
                peers.append(peer)
                return peer
            try:
                _, old, _ = wire.upload(connect(), 'a', b'aaaabbbb', [[0, 2]])
                _, old_b, _ = wire.upload(connect(), 'b', b'aaaabbbb', [[0, 2]])
                p = connect()
                ready = prepare(p, 'a')
                sid = ready['session_id']
                other = connect()
                other.ask(28, dict(session_id=sid), 29)
                q = connect()
                second = prepare(q, 'b') if scenario == 'parallel' else None
                operation, target = 'pread', '/.delta/' + sid
                if scenario == 'baseline':
                    target = '/files/a'
                if scenario in ('object_copy', 'copy_error', 'prepare_sql', 'crash_prepared'):
                    operation, target = 'pwrite', '/.versions/'
                if scenario in ('publish_copy', 'publish_sql', 'runtime_retry', 'crash_published',
                                'publish_pointer_race', 'publish_target_race'):
                    operation, target = 'pwrite', '/.delta/.publish-' + sid
                if scenario == 'object_sync':
                    operation, target = 'fsync', '/.versions/'
                if scenario in ('publish_sync', 'sync_error'):
                    operation, target = 'fsync', '/files'
                gate.write_text(operation + '\n' + target)
                if scenario == 'lost_response':
                    p.sock = DropCommitReply(p.sock)
                slow = pool.submit(p.ask, 30, dict(session_id=sid), 31)
                wait_for(lambda: entered.exists(), 'commit barrier was not reached')
                if scenario in ('hash', 'baseline', 'object_copy', 'publish_copy', 'object_sync', 'publish_sync'):
                    independent = connect()
                    independent.sock.settimeout(1)
                    started = time.monotonic()
                    new = wire.begin(independent, 'independent', b'new!')
                    assert independent.ask(28, dict(session_id=new['session_id']), 29)['phase'] == 'COLLECTING'
                    print(scenario, 'independent requests before release', time.monotonic() - started, flush=True)
                if scenario in ('duplicate', 'query', 'write'):
                    other.sock.settimeout(1)
                    if scenario == 'duplicate':
                        assert other.ask(30, dict(session_id=sid), 15)['code'] == 'BUSY'
                    elif scenario == 'query':
                        assert other.ask(28, dict(session_id=sid), 15)['code'] == 'BUSY'
                    else:
                        body = bytes.fromhex(sid) + wire.struct.pack('!QI', 4, 4) + wire.hashlib.sha256(b'bbbb').digest() + b'bbbb'
                        assert other.ask(5, body, 15)['code'] == 'BUSY'
                if scenario == 'parallel':
                    assert q.ask(30, dict(session_id=second['session_id']), 31)['committed']
                if scenario == 'path_conflict':
                    contender = connect()
                    meta = dict(path='a', size=8, mtime_ns=0, chunk_size=4, sha256=wire.digest(b'xxxxbbbb'))
                    assert contender.ask(24, dict(metadata=meta, mode='upload',
                                                base_version=old['version_id']), 15)['code'] == 'CONFLICT'
                if scenario == 'expiry':
                    with sqlite3.connect(root / 'metadata.sqlite3') as db:
                        db.execute('UPDATE delta_sessions SET created=0 WHERE id=?', (sid,))
                    independent = connect()
                    wire.begin(independent, 'independent', b'new!')
                    assert (root / '.delta' / sid).exists()
                if scenario in ('prepare_sql', 'publish_sql', 'runtime_retry'):
                    with sqlite3.connect(root / 'metadata.sqlite3') as db:
                        phase = 4 if scenario == 'prepare_sql' else 5
                        db.execute(f"CREATE TRIGGER reject_commit BEFORE UPDATE OF phase ON delta_sessions "
                                   f"WHEN NEW.phase={phase} BEGIN SELECT RAISE(ABORT,'test'); END")
                if scenario in ('pointer_race', 'publish_pointer_race'):
                    # Deliberate DB corruption injection tests the final CAS boundary.
                    with sqlite3.connect(root / 'metadata.sqlite3') as db:
                        db.execute("UPDATE current_files SET version_id=? WHERE path='a'", (old_b['version_id'],))
                if scenario in ('target_race', 'publish_target_race'):
                    replacement = root / 'files' / 'replacement'
                    replacement.write_bytes(b'aaaabbbb')
                    replacement.chmod(0o600)
                    replacement.replace(root / 'files' / 'a')
                if scenario == 'checksum_error':
                    with (root / '.delta' / sid).open('r+b') as stream:
                        stream.write(b'bad!')
                if scenario == 'disconnect':
                    p.close()
                if scenario in ('crash_prepared', 'crash_published'):
                    wire.stop(server, crash=True)
                    server = None
                    try:
                        slow.result(timeout=3)
                    except (AssertionError, OSError):
                        pass
                    gate.unlink()
                    server, port = wire.start(root, env)
                if scenario in ('copy_error', 'sync_error'):
                    Path(str(gate) + '.fail').touch()
                release.touch()
                failed = scenario in ('copy_error', 'sync_error', 'prepare_sql', 'publish_sql', 'runtime_retry',
                                      'pointer_race', 'target_race', 'checksum_error',
                                      'publish_pointer_race', 'publish_target_race')
                try:
                    result = slow.result(timeout=5)
                    assert not failed
                except (AssertionError, OSError) as error:
                    assert failed or scenario in ('disconnect', 'lost_response',
                                                   'crash_prepared', 'crash_published'), error
                if scenario == 'lost_response':
                    assert p.sock.dropped
                state, current, integrity, foreign = rows(root, sid)
                assert integrity == 'ok' and foreign == []
                assert (root / '.versions' / old['version_id'] / 'data').read_bytes() == b'aaaabbbb'
                if failed:
                    assert state[0] == (4 if scenario in ('sync_error', 'publish_sql', 'runtime_retry',
                                                        'publish_pointer_race', 'publish_target_race') else 2)
                    assert current == (old_b['version_id'] if scenario in ('pointer_race', 'publish_pointer_race')
                                       else old['version_id'])
                    with sqlite3.connect(root / 'metadata.sqlite3') as db:
                        expected_versions = 2 if state[0] == 4 else 1
                        assert db.execute("SELECT count(*) FROM versions WHERE path='a'").fetchone()[0] == expected_versions
                        assert db.execute("SELECT count(*) FROM version_history WHERE path='a'").fetchone()[0] == 1
                    assert (root / '.delta' / sid).exists()
                    if scenario == 'copy_error':
                        pending_objects = list((root / '.versions').glob('*/pending'))
                        assert len(pending_objects) == 1 and pending_objects[0].read_bytes() == b''
                    if state[0] == 4:
                        assert (root / '.versions' / state[1] / 'data').read_bytes() == b'zzzzbbbb'
                        # Filesystem publication can precede the final SQL commit.
                        expected_file = b'aaaabbbb' if scenario in ('publish_pointer_race', 'publish_target_race') else b'zzzzbbbb'
                        assert (root / 'files' / 'a').read_bytes() == expected_file
                    else:
                        assert (root / 'files' / 'a').read_bytes() == b'aaaabbbb'
                    if scenario in ('pointer_race', 'publish_pointer_race'):
                        with sqlite3.connect(root / 'metadata.sqlite3') as db:
                            db.execute("UPDATE current_files SET version_id=? WHERE path='a'", (old['version_id'],))
                    if scenario in ('prepare_sql', 'publish_sql', 'runtime_retry'):
                        with sqlite3.connect(root / 'metadata.sqlite3') as db:
                            db.execute('DROP TRIGGER reject_commit')
                    Path(str(gate) + '.fail').unlink(missing_ok=True)
                    # Reopen the same root to exercise durable recovery.
                    if scenario == 'runtime_retry':
                        assert connect().ask(28, dict(session_id=sid), 15)['code'] == 'BUSY'
                    else:
                        wire.stop(server)
                        server = None
                        gate.unlink()
                        server, port = wire.start(root, env)
                        if scenario in ('copy_error', 'prepare_sql', 'pointer_race', 'target_race'):
                            assert not list((root / '.quarantine').glob('version-*'))
                            assert not list((root / '.preparations').iterdir())
                retry = connect()
                if scenario == 'checksum_error':
                    assert retry.ask(28, dict(session_id=sid), 29)['missing_bitmap'] == '01'
                    wire.send_block(retry, sid, 0, b'zzzz')
                recovered = retry.ask(30, dict(session_id=sid), 31)
                assert recovered['sha256'] == wire.digest(b'zzzzbbbb')
                assert retry.ask(30, dict(session_id=sid), 31) == recovered
                status = connect().ask(28, dict(session_id=sid), 29)
                assert status['phase'] == 'COMMITTED' and status['result'] == recovered
                state, current, _, _ = rows(root, sid)
                assert state[0] == 5 and state[1] == current == recovered['version_id']
                assert (root / 'files' / 'a').read_bytes() == b'zzzzbbbb'
                assert not (root / '.delta' / sid).exists()
                with sqlite3.connect(root / 'metadata.sqlite3') as db:
                    assert db.execute('SELECT count(*) FROM version_history WHERE version_id=?',
                                      (current,)).fetchone()[0] == 1
                if scenario == 'path_conflict':
                    stale = connect()
                    assert stale.ask(24, dict(metadata=meta, mode='upload',
                                             base_version=old['version_id']), 15)['code'] == 'CONFLICT'
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
