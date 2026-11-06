"""Real server barrier tests for unlocked, immutable-baseline planning."""
import concurrent.futures
import os
from pathlib import Path
import sqlite3
import sys
import tempfile
import time

import delta_protocol_test as wire


def wait_for(predicate, message):
    until = time.monotonic() + 4
    while time.monotonic() < until:
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError(message)


def prepare(peer, path, data, mode='preview'):
    ready = wire.begin(peer, path, data, mode=mode)
    hashes = [wire.digest(data[i:i + 4]) for i in range(0, len(data), 4)]
    peer.ask(18, dict(session_id=ready['session_id'], page_index=0,
                     first_block_index=0, hashes=hashes), 26)
    return ready


def finish(peer, ready, expected=26):
    return peer.ask(19, dict(session_id=ready['session_id'], page_count=1,
                            block_count=ready['block_count']), expected)


def main():
    hook = sys.argv[2]
    for scenario in ('progress', 'parallel', 'duplicate', 'disconnect', 'crash', 'io_error', 'install_error', 'expiry'):
        with tempfile.TemporaryDirectory(prefix='chunkharbor-plan-lock-') as tmp:
            root = Path(tmp) / 'backup'
            root.mkdir(mode=0o700)
            gate = Path(tmp) / 'gate'
            release = Path(str(gate) + '.release')
            entered = Path(str(gate) + '.entered')
            env = dict(os.environ, LD_PRELOAD=hook, CH_PLAN_TEST_GATE=str(gate))
            server, port = wire.start(root, env)
            pool = concurrent.futures.ThreadPoolExecutor(max_workers=3)
            peers = []
            try:
                first = wire.Peer(port)
                peers.append(first)
                _, version, _ = wire.upload(first, 'test/a', b'aaaabbbb', [[0, 2]])
                other = wire.Peer(port)
                peers.append(other)
                wire.upload(other, 'test/b', b'ccccdddd', [[0, 2]])
                other.close()
                other = wire.Peer(port)
                peers.append(other)
                p = wire.Peer(port)
                peers.append(p)
                ready = prepare(p, 'test/a', b'aaaaxxxx')
                # Select all version FDs only for the distinct-file parallel case.
                gate.write_text('/.versions/' if scenario == 'parallel' else version['version_id'])
                slow = pool.submit(finish, p, ready)
                wait_for(lambda: entered.exists(), 'planner never reached baseline pread')
                if scenario == 'progress':
                    # Must finish while the baseline read remains blocked.
                    other.sock.settimeout(1)
                    started = time.monotonic()
                    pending = wire.begin(other, 'test/new', b'new!')
                    state = other.ask(28, dict(session_id=pending['session_id']), 29)
                    assert state['phase'] == 'COLLECTING'
                    # Publish B at the same path while preview remains pinned to A.
                    writer = wire.Peer(port)
                    peers.append(writer)
                    _, newer, _ = wire.upload(writer, 'test/a', b'zzzzbbbb', [[0, 1]], chunk_size=8)
                    writer.close()
                    other = wire.Peer(port)
                    peers.append(other)
                    assert newer['version_id'] != version['version_id']
                    print('independent BEGIN/QUERY and same-path publication progressed in',
                          round(time.monotonic() - started, 3), 'seconds before release', flush=True)
                    stale = dict(metadata=dict(path='test/a', size=8, mtime_ns=0,
                                 chunk_size=4, sha256=wire.digest(b'aaaaxxxx')),
                                 mode='upload', base_version=version['version_id'])
                    assert other.ask(24, stale, 15)['code'] == 'CONFLICT'
                    # Upload path reservation remains exclusive.
                    contender = wire.Peer(port)
                    peers.append(contender)
                    wire.begin(contender, 'test/busy', b'busy')
                    other = wire.Peer(port)
                    peers.append(other)
                    meta = dict(path='test/busy', size=4, mtime_ns=0, chunk_size=4,
                                sha256=wire.digest(b'busy'))
                    assert other.ask(24, dict(metadata=meta, mode='upload',
                                             base_version=None), 15)['code'] == 'CONFLICT'
                if scenario in ('parallel', 'duplicate'):
                    q = wire.Peer(port)
                    peers.append(q)
                    second = prepare(q, 'test/b', b'ccccxxxx') if scenario == 'parallel' else ready
                    if scenario == 'duplicate':
                        q.ask(28, dict(session_id=ready['session_id']), 29)
                    second_plan = pool.submit(finish, q, second)
                    wait_for(lambda: entered.stat().st_size >= 2, 'second planner blocked by shared lock')
                if scenario == 'disconnect':
                    p.close()
                if scenario == 'expiry':
                    dbfile = root / 'metadata.sqlite3'
                    with sqlite3.connect(dbfile) as db:
                        db.execute('UPDATE delta_sessions SET created=0 WHERE id=?', (ready['session_id'],))
                if scenario == 'install_error':
                    with sqlite3.connect(root / 'metadata.sqlite3') as db:
                        db.execute("CREATE TRIGGER reject_plan BEFORE UPDATE OF phase ON delta_sessions "
                                   "WHEN NEW.phase=1 BEGIN SELECT RAISE(ABORT, 'plan failure'); END")
                if scenario == 'crash':
                    wire.stop(server, crash=True)
                    server = None
                    try:
                        slow.result(timeout=3)
                    except (AssertionError, OSError):
                        pass
                    gate.unlink()
                    server, port = wire.start(root, env)
                    retry = wire.Peer(port)
                    peers.append(retry)
                    state = retry.ask(28, dict(session_id=ready['session_id']), 29)
                    assert state['phase'] == 'COLLECTING' and state['next_manifest_page'] == 1
                    assert finish(retry, ready)['phase'] == 'PLAN_READY'
                    wire.plan(retry, ready, [[1, 2]])
                else:
                    if scenario == 'io_error':
                        Path(str(gate) + '.fail').touch()
                    release.touch()
                    if scenario == 'disconnect':
                        try:
                            slow.result(timeout=3)
                        except (OSError, AssertionError):
                            pass
                        wait_for(lambda: other.ask(28, dict(session_id=ready['session_id']), 29)['phase']
                                 == 'PLAN_READY', 'disconnected planner did not persist plan')
                    elif scenario == 'expiry':
                        assert slow.result(timeout=3)['phase'] == 'PLAN_READY'
                        assert other.ask(28, dict(session_id=ready['session_id']), 15)['code'] == 'SESSION_NOT_FOUND'
                        reaper = wire.Peer(port)
                        peers.append(reaper)
                        wire.begin(reaper, 'reap', b'data')
                    elif scenario in ('io_error', 'install_error'):
                        try:
                            slow.result(timeout=3)
                            raise AssertionError('expected failed planning')
                        except AssertionError as error:
                            assert 'INTERNAL_ERROR' in str(error) or 'SESSION_NOT_FOUND' in str(error), error
                        if scenario in ('io_error', 'install_error'):
                            with sqlite3.connect(root / 'metadata.sqlite3') as db:
                                assert db.execute('SELECT count(*) FROM delta_plan_ranges WHERE session=?',
                                                  (ready['session_id'],)).fetchone()[0] == 0
                                assert db.execute('SELECT sum(missing) FROM manifest_chunks WHERE session=?',
                                                  (ready['session_id'],)).fetchone()[0] == ready['block_count']
                                if scenario == 'install_error':
                                    db.execute('DROP TRIGGER reject_plan')
                            if scenario == 'io_error':
                                Path(str(gate) + '.fail').unlink()
                            assert other.ask(28, dict(session_id=ready['session_id']), 29)['phase'] == 'COLLECTING'
                            assert finish(other, ready)['phase'] == 'PLAN_READY'
                            wire.plan(other, ready, [[1, 2]])
                    else:
                        assert slow.result(timeout=3)['phase'] == 'PLAN_READY'
                        wire.plan(p, ready, [[1, 2]])
                        if scenario in ('parallel', 'duplicate'):
                            assert second_plan.result(timeout=3)['phase'] == 'PLAN_READY'
                    if scenario == 'expiry':
                        with sqlite3.connect(dbfile) as db:
                            assert db.execute('SELECT count(*) FROM delta_plan_ranges WHERE session=?',
                                              (ready['session_id'],)).fetchone()[0] == 0
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
