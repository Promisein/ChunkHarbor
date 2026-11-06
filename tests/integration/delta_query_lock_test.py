"""Deterministic resume-query I/O isolation on a disposable real server."""
import concurrent.futures
import os
from pathlib import Path
import sqlite3
import sys
import tempfile
import time

import delta_protocol_test as wire
from delta_plan_lock_test import wait_for


def query(peer, sid):
    return peer.ask(28, dict(session_id=sid), 29)


def main():
    scenarios = ('progress', 'duplicate', 'write', 'commit', 'disconnect',
                 'restart', 'corrupt', 'io_error', 'missing', 'expiry', 'cleanup', 'rollback')
    for scenario in scenarios:
        with tempfile.TemporaryDirectory(prefix='chunkharbor-query-') as tmp:
            root = Path(tmp) / 'backup'
            root.mkdir(mode=0o700)
            gate = Path(tmp) / 'gate'
            release = Path(str(gate) + '.release')
            entered = Path(str(gate) + '.entered')
            env = dict(os.environ, LD_PRELOAD=sys.argv[2], CH_QUERY_TEST_GATE=str(gate))
            server, port = wire.start(root, env)
            peers = []
            pool = concurrent.futures.ThreadPoolExecutor(max_workers=1)
            def connect():
                peer = wire.Peer(port)
                peers.append(peer)
                return peer
            try:
                writer = connect()
                ready = wire.begin(writer, 'a', b'aaaabbbb')
                sid = ready['session_id']
                wire.manifest(writer, ready, b'aaaabbbb')
                wire.plan(writer, ready, [[0, 2]])
                wire.send_block(writer, sid, 0, b'aaaa')
                if scenario == 'commit':
                    wire.send_block(writer, sid, 4, b'bbbb')
                stage = root / '.delta' / sid
                if scenario == 'missing':
                    stage.unlink()
                    failed = connect()
                    assert failed.ask(28, dict(session_id=sid), 15)['code'] == 'INTERNAL_ERROR'
                    with sqlite3.connect(root / 'metadata.sqlite3') as db:
                        assert db.execute('SELECT sum(accepted) FROM manifest_chunks WHERE session=?',
                                          (sid,)).fetchone()[0] == 1
                    stage.write_bytes(b'aaaa' + bytes(4))
                    stage.chmod(0o600)
                    assert query(connect(), sid)['missing_bitmap'] == '02'
                    print(scenario, 'PASS', flush=True)
                    continue
                if scenario in ('corrupt', 'rollback'):
                    with stage.open('r+b') as stream:
                        stream.write(b'xxxx')
                if scenario == 'rollback':
                    with sqlite3.connect(root / 'metadata.sqlite3') as db:
                        db.execute("CREATE TRIGGER reject_clear BEFORE UPDATE OF accepted ON manifest_chunks "
                                   "WHEN NEW.accepted=0 BEGIN SELECT RAISE(ABORT,'test'); END")
                gate.write_text(sid)
                reader = connect()
                slow = pool.submit(query, reader, sid)
                wait_for(lambda: entered.exists(), 'query never reached accepted-block pread')
                if scenario == 'progress':
                    other = connect()
                    other.sock.settimeout(1)
                    started = time.monotonic()
                    new = wire.begin(other, 'independent', b'new!')
                    assert query(other, new['session_id'])['phase'] == 'COLLECTING'
                    print('independent BEGIN/QUERY before release', time.monotonic() - started, flush=True)
                if scenario == 'duplicate':
                    other = connect()
                    other.sock.settimeout(1)
                    assert other.ask(28, dict(session_id=sid), 15)['code'] == 'BUSY'
                if scenario == 'write':
                    body = bytes.fromhex(sid) + wire.struct.pack('!QI', 4, 4) + wire.hashlib.sha256(b'bbbb').digest() + b'bbbb'
                    writer.sock.settimeout(1)
                    assert writer.ask(5, body, 15)['code'] == 'BUSY'
                if scenario == 'commit':
                    writer.sock.settimeout(1)
                    assert writer.ask(30, dict(session_id=sid), 15)['code'] == 'BUSY'
                if scenario == 'disconnect':
                    reader.close()
                if scenario in ('expiry', 'cleanup'):
                    with sqlite3.connect(root / 'metadata.sqlite3') as db:
                        db.execute('UPDATE delta_sessions SET created=0 WHERE id=?', (sid,))
                    if scenario == 'cleanup':
                        other = connect()
                        wire.begin(other, 'other', b'new!')
                        assert stage.exists()
                if scenario == 'restart':
                    wire.stop(server, crash=True)
                    server = None
                    try:
                        slow.result(timeout=3)
                    except (AssertionError, OSError):
                        pass
                    gate.unlink()
                    server, port = wire.start(root, env)
                    assert query(connect(), sid)['missing_bitmap'] == '02'
                else:
                    if scenario == 'io_error':
                        Path(str(gate) + '.fail').touch()
                    release.touch()
                    if scenario in ('expiry', 'cleanup'):
                        assert slow.result(timeout=3)['missing_bitmap'] == '02'
                        assert connect().ask(28, dict(session_id=sid), 15)['code'] == 'SESSION_NOT_FOUND'
                        wire.begin(connect(), 'reap', b'data')
                        assert not stage.exists()
                    elif scenario == 'rollback':
                        try:
                            slow.result(timeout=3)
                            raise AssertionError('expected rejected query')
                        except AssertionError as error:
                            assert 'SESSION_NOT_FOUND' in str(error) or 'INTERNAL_ERROR' in str(error), error
                        if scenario == 'rollback':
                            with sqlite3.connect(root / 'metadata.sqlite3') as db:
                                assert db.execute('SELECT sum(accepted) FROM manifest_chunks WHERE session=?',
                                                  (sid,)).fetchone()[0] == 1
                                db.execute('DROP TRIGGER reject_clear')
                            assert query(connect(), sid)['missing_bitmap'] == '03'
                    elif scenario == 'disconnect':
                        try:
                            slow.result(timeout=3)
                        except (AssertionError, OSError):
                            pass
                        assert query(connect(), sid)['missing_bitmap'] == '02'
                    else:
                        result = slow.result(timeout=3)
                        expected = '03' if scenario in ('corrupt', 'io_error') else '00' if scenario == 'commit' else '02'
                        assert result['missing_bitmap'] == expected
                        assert query(connect(), sid)['missing_bitmap'] == expected
                if scenario not in ('missing', 'expiry', 'cleanup'):
                    if scenario == 'io_error':
                        Path(str(gate) + '.fail').unlink()
                    retry = connect()
                    query(retry, sid)
                    if scenario in ('corrupt', 'io_error', 'rollback'):
                        wire.send_block(retry, sid, 0, b'aaaa')
                    if scenario != 'commit':
                        wire.send_block(retry, sid, 4, b'bbbb')
                    assert retry.ask(30, dict(session_id=sid), 31)['sha256'] == wire.digest(b'aaaabbbb')
                    assert query(connect(), sid)['phase'] == 'COMMITTED'
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
