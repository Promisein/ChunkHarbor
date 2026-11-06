"""A.5.3 supplementary measurements and two-worker saturation acceptance.

Existing A.1-A.5.2 suites own the detailed fault matrix. This file adds only
metadata/cleanup cost samples and distinguishes worker queueing from lock waits.
"""
import concurrent.futures
import json
import os
from pathlib import Path
import select
import sqlite3
import subprocess
import sys
import tempfile
import time

import delta_protocol_test as wire
from delta_plan_lock_test import prepare, finish, wait_for
from legacy_index_lock_test import old_upload, indexed


def send(peer, kind, value):
    peer.request_id += 1
    body = json.dumps(value, separators=(',', ':')).encode()
    peer.sock.sendall(wire.HEADER.pack(0x43484B48, 1, kind, 0, peer.request_id, len(body), 0) + body)


def receive(peer, expected):
    header = wire.HEADER.unpack(wire.exact(peer.sock, 32))
    assert header[2] == expected and header[4] == peer.request_id, header
    return json.loads(wire.exact(peer.sock, header[5]))


def check_db(root):
    with sqlite3.connect(root / 'metadata.sqlite3') as db:
        assert db.execute('PRAGMA integrity_check').fetchone() == ('ok',)
        assert not db.execute('PRAGMA foreign_key_check').fetchall()
        for path, version, sha in db.execute(
                'SELECT c.path,v.id,v.sha256 FROM current_files c JOIN versions v ON v.id=c.version_id'):
            assert wire.digest((root / '.versions' / version / 'data').read_bytes()) == sha
            assert wire.digest((root / 'files' / path).read_bytes()) == sha


def metadata_cost(root, port, trace):
    # End-to-end BEGIN is an upper bound on index SQL lock cost, not an exact profiler.
    for count in (0, 4096, 65536):
        data = b'a' * count
        path = 'index-' + str(count)
        sid = old_upload(port, path, data, chunk=65536)
        with sqlite3.connect(root / 'metadata.sqlite3') as db:
            db.execute('UPDATE transfers SET chunk_size=1 WHERE session_id=?', (sid,))
            db.execute('DELETE FROM transfer_chunks WHERE session_id=?', (sid,))
            db.executemany('INSERT INTO transfer_chunks VALUES(?,?,?,1)',
                           ((sid, i, wire.digest(b'a')) for i in range(count)))
        peer = wire.Peer(port)
        try:
            trace.write_text('')
            started = time.monotonic()
            assert wire.begin(peer, path, data, chunk_size=1, mode='preview')['base_version'] == sid
            elapsed = time.monotonic() - started
            operation, status, seconds = trace.read_text().strip().split()
            sql_seconds = float(seconds)
            assert operation == 'COMMIT' and status == '0' and 0 <= sql_seconds <= elapsed + 0.01
            indexed(root, sid, data, chunk=1)
            print('metadata BEGIN upper bound:', count, 'blocks', round(elapsed, 6), 'seconds', flush=True)
            print('legacy SQL transaction:', count, 'blocks', sql_seconds, 'seconds', flush=True)
        finally:
            peer.close()
    check_db(root)


def cleanup_cost(root, port):
    peer = wire.Peer(port)
    try:
        _, anchor, _ = wire.upload(peer, 'anchor', b'aaaabbbb', [[0, 2]])
    finally:
        peer.close()
    peer = wire.Peer(port)
    try:
        old_preview = wire.begin(peer, 'priority-preview', b'', mode='preview')
        wire.manifest(peer, old_preview, b'')
        wire.plan(peer, old_preview, [])
    finally:
        peer.close()
    ids = []
    for index in range(32):
        peer = wire.Peer(port)
        try:
            ready = wire.begin(peer, 'expired-' + str(index), b'ccccdddd')
            wire.manifest(peer, ready, b'ccccdddd')
            wire.plan(peer, ready, [[0, 2]])
            ids.append(ready['session_id'])
        finally:
            peer.close()
    assert all((root / '.delta' / sid).exists() for sid in ids)
    with sqlite3.connect(root / 'metadata.sqlite3') as db:
        row = db.execute('SELECT request,base,logical_path,mode,phase FROM delta_sessions WHERE id=?',
                         (old_preview['session_id'],)).fetchone()
        db.executemany('INSERT INTO delta_sessions(id,request,base,logical_path,mode,phase,created) '
                       'VALUES(?,?,?,?,?,?,0)', ((os.urandom(16).hex(), *row) for _ in range(63)))
        db.execute('UPDATE delta_sessions SET created=0 WHERE mode=1')
        db.executemany('UPDATE delta_sessions SET created=0 WHERE id=?', ((sid,) for sid in ids))
    peer = wire.Peer(port)
    try:
        started = time.monotonic()
        wire.begin(peer, 'after-cleanup', b'data')
        print('cleanup BEGIN upper bound: 32 staged sessions', round(time.monotonic() - started, 6),
              'seconds', flush=True)
        with sqlite3.connect(root / 'metadata.sqlite3') as db:
            assert db.execute('SELECT count(*) FROM delta_sessions WHERE mode=1').fetchone() == (64,)
            assert all(db.execute('SELECT 1 FROM delta_sessions WHERE id=?', (sid,)).fetchone() is None
                       for sid in ids)
        assert all(not (root / '.delta' / sid).exists() for sid in ids)
        assert (root / '.versions' / anchor['version_id'] / 'data').read_bytes() == b'aaaabbbb'
        check_db(root)
    finally:
        peer.close()


def preview_backlog(root, port, marker):
    # Existing preview cap, not an R8 stress scale. Empty completed plans are valid.
    control, template = wire.Peer(port), wire.Peer(port)
    pool = concurrent.futures.ThreadPoolExecutor(max_workers=1)
    peers = [control, template]
    try:
        active = wire.begin(control, 'control', b'data')
        preview = wire.begin(template, 'old-preview', b'', mode='preview')
        wire.manifest(template, preview, b'')
        wire.plan(template, preview, [])
        with sqlite3.connect(root / 'metadata.sqlite3') as db:
            row = db.execute('SELECT request,base,logical_path,mode,phase FROM delta_sessions WHERE id=?',
                             (preview['session_id'],)).fetchone()
            db.executemany('INSERT INTO delta_sessions(id,request,base,logical_path,mode,phase,created) '
                           'VALUES(?,?,?,?,?,?,0)', ((os.urandom(16).hex(), *row) for _ in range(19999)))
            db.execute("UPDATE delta_sessions SET created=strftime('%s','now') WHERE mode=1")
        Path(str(marker) + '.arm').touch()
        fresh_trigger = wire.Peer(port)
        peers.append(fresh_trigger)
        fresh_task = pool.submit(wire.begin, fresh_trigger, 'fresh-trigger', b'data')
        wait_for(marker.exists, 'fresh preview scan not entered under shared locks')
        control.sock.settimeout(1)
        queried = time.monotonic()
        assert control.ask(28, dict(session_id=active['session_id']), 29)['phase'] == 'COLLECTING'
        fresh_delay = time.monotonic() - queried
        assert fresh_delay < 1
        fresh_task.result(timeout=60)
        with sqlite3.connect(root / 'metadata.sqlite3') as db:
            assert db.execute('SELECT count(*) FROM delta_sessions WHERE mode=1').fetchone() == (20000,)
            db.execute('UPDATE delta_sessions SET created=0 WHERE mode=1')
        marker.unlink()
        print('fresh preview backlog: 20000 preserved, independent query', round(fresh_delay, 6),
              'seconds', flush=True)
        trigger = wire.Peer(port)
        peers.append(trigger)
        trigger.sock.settimeout(60)
        started = time.monotonic()
        task = pool.submit(wire.begin, trigger, 'cleanup-trigger', b'data')
        def started_cleaning():
            with sqlite3.connect(root / 'metadata.sqlite3') as db:
                return db.execute('SELECT count(*) FROM delta_sessions WHERE mode=1').fetchone()[0] < 20000
        wait_for(started_cleaning, 'preview cleanup not entered')
        control.sock.settimeout(1)
        queried = time.monotonic()
        assert control.ask(28, dict(session_id=active['session_id']), 29)['phase'] == 'COLLECTING'
        delay = time.monotonic() - queried
        assert delay < 1, ('preview backlog blocks independent query', delay)
        task.result(timeout=60)
        with sqlite3.connect(root / 'metadata.sqlite3') as db:
            assert db.execute('SELECT count(*) FROM delta_sessions WHERE mode=1').fetchone() == (19968,)
        print('preview backlog: 20000 -> 19968, independent query', round(delay, 6),
              'seconds; BEGIN total', round(time.monotonic() - started, 6), flush=True)
    finally:
        pool.shutdown(wait=True)
        for peer in peers:
            peer.close()


def start_large_recovery(root, env):
    # Full, serialized startup may process the whole 20k backlog; this deadline is
    # only for the test observer. No product request/idle timeout is changed.
    server = subprocess.Popen([wire.SERVER, '--upload', '--root', str(root), '--port', '0'],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    pool = concurrent.futures.ThreadPoolExecutor(max_workers=1)
    try:
        line = pool.submit(server.stdout.readline).result(timeout=60).strip()
        assert line.startswith('LISTENING 127.0.0.1:'), line
        return server, int(line.rsplit(':', 1)[1])
    except BaseException:
        server.kill()
        server.communicate(timeout=10)
        raise
    finally:
        pool.shutdown(wait=True)


def saturation(root, server, port, env, gate, shutdown):
    peers = []
    release = Path(str(gate) + '.release')
    entered = Path(str(gate) + '.entered')
    pool = concurrent.futures.ThreadPoolExecutor(max_workers=2)
    def connect():
        p = wire.Peer(port)
        peers.append(p)
        return p
    fd_initial = len(list(Path('/proc/' + str(server.pid) + '/fd').iterdir()))
    try:
        wire.upload(connect(), 'a', b'aaaabbbb', [[0, 2]])
        wire.upload(connect(), 'b', b'ccccdddd', [[0, 2]])
        a, b, queued = connect(), connect(), connect()
        ra = prepare(a, 'a', b'aaaaxxxx')
        rb = prepare(b, 'b', b'ccccxxxx')
        gate.write_text('pread\n/.versions/')
        tasks = [pool.submit(finish, a, ra), pool.submit(finish, b, rb)]
        wait_for(lambda: entered.exists() and entered.stat().st_size >= 2, 'both workers not in slow I/O')
        # Both workers are now held outside storage locks and outside SQL transactions.
        with sqlite3.connect(root / 'metadata.sqlite3', timeout=1) as db:
            db.execute('BEGIN IMMEDIATE')
            db.rollback()
        send(queued, 24, dict(metadata=dict(path='queued', size=4, mtime_ns=0,
                                          chunk_size=4, sha256=wire.digest(b'data')),
                             mode='preview', base_version='auto'))
        assert not select.select([queued.sock], [], [], 0.1)[0], 'third disk job bypassed occupied workers'
        # HELLO is handled by Reactor, and still completes while disk jobs cannot run.
        started = time.monotonic()
        connect()
        assert time.monotonic() - started < 1, 'Reactor heartbeat blocked by workers'
        print('saturated worker waits:', [(p.name, (p / 'wchan').read_text())
              for p in Path('/proc/' + str(server.pid) + '/task').iterdir()], flush=True)
        if shutdown:
            queued.close()
            a.close()
            server.terminate()
            # SIGTERM must already have left Reactor and entered worker join.
            # Both workers remain gated until after this assertion.
            main_wait = Path('/proc/' + str(server.pid) + '/task/' + str(server.pid) + '/wchan')
            wait_for(lambda: server.poll() is None and 'futex' in main_wait.read_text(),
                     'shutdown did not join blocked workers before release')
        release.touch()
        for task in tasks:
            try:
                assert task.result(timeout=4)['phase'] == 'PLAN_READY'
            except (OSError, AssertionError):
                if not shutdown:
                    raise
        if shutdown:
            server.communicate(timeout=4)
            assert server.returncode == 0
            with sqlite3.connect(root / 'metadata.sqlite3') as db:
                for ready in (ra, rb):
                    assert db.execute('SELECT phase FROM delta_sessions WHERE id=?',
                                      (ready['session_id'],)).fetchone() == (1,)
                    assert db.execute('SELECT idx,missing FROM manifest_chunks WHERE session=? ORDER BY idx',
                                      (ready['session_id'],)).fetchall() == [(0, 0), (1, 1)]
        else:
            ready = receive(queued, 25)
            assert ready['base_version'] is None
            # Pin release is observed through real expiration, not internal counters.
            with sqlite3.connect(root / 'metadata.sqlite3') as db:
                db.executemany('UPDATE delta_sessions SET created=0 WHERE id=?',
                               ((r['session_id'],) for r in (ra, rb, ready)))
            wire.begin(connect(), 'after-saturation', b'data')
            with sqlite3.connect(root / 'metadata.sqlite3') as db:
                assert all(db.execute('SELECT 1 FROM delta_sessions WHERE id=?',
                                      (r['session_id'],)).fetchone() is None for r in (ra, rb, ready))
        check_db(root)
    finally:
        release.touch()
        pool.shutdown(wait=True)
        for p in peers:
            p.close()
    if not shutdown:
        wait_for(lambda: len(list(Path('/proc/' + str(server.pid) + '/fd').iterdir())) <= fd_initial,
                 'FDs did not return after saturation')


def main():
    for case in ('metadata_cost', 'cleanup_cost', 'saturation', 'saturation_shutdown', 'preview_backlog'):
        with tempfile.TemporaryDirectory(prefix='chunkharbor-a53-') as tmp:
            root = Path(tmp) / 'backup'
            root.mkdir(mode=0o700)
            gate = Path(tmp) / 'gate'
            trace = Path(tmp) / 'sql-cost'
            expiry_marker = Path(tmp) / 'expiry-entered'
            env = dict(os.environ, LD_PRELOAD=sys.argv[2], CH_COMMIT_TEST_GATE=str(gate),
                       CH_A53_SQL_COST=str(trace), CH_A53_EXPIRY_MARKER=str(expiry_marker))
            server, port = wire.start(root, env)
            try:
                if case == 'metadata_cost':
                    metadata_cost(root, port, trace)
                elif case == 'cleanup_cost':
                    cleanup_cost(root, port)
                elif case == 'preview_backlog':
                    preview_backlog(root, port, expiry_marker)
                else:
                    saturation(root, server, port, env, gate, case.endswith('shutdown'))
                print(case, 'PASS', flush=True)
            finally:
                Path(str(gate) + '.release').touch()
                wire.stop(server)
            # Recovery is serialized before serving; repeated starts must preserve DB/FS.
            before = sorted(p.name for p in (root / '.versions').iterdir())
            for attempt in range(3):
                started = time.monotonic()
                server, port = (start_large_recovery(root, env) if case == 'preview_backlog'
                                else wire.start(root, env))
                print(case, 'restart', attempt, 'ready seconds', round(time.monotonic() - started, 6), flush=True)
                wire.stop(server)
                assert sorted(p.name for p in (root / '.versions').iterdir()) == before
                check_db(root)
                if case == 'preview_backlog':
                    with sqlite3.connect(root / 'metadata.sqlite3') as db:
                        assert db.execute('SELECT count(*) FROM delta_sessions WHERE mode=1').fetchone() == (0,)
            assert not list((root / '.preparations').iterdir())


if __name__ == '__main__':
    main()
