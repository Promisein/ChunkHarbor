"""Materialization owns one staging file while other requests make progress."""
import concurrent.futures
import os
from pathlib import Path
import sqlite3
import sys
import tempfile
import time

import delta_protocol_test as wire
from delta_plan_lock_test import wait_for


def pending(peer, path):
    ready = wire.begin(peer, path, b'aaaaxxxx')
    wire.manifest(peer, ready, b'aaaaxxxx')
    assert peer.ask(27, dict(session_id=ready['session_id'], page_index=0), 20)['missing_ranges'] == [[1, 2]]
    return ready


def final_page(peer, ready):
    return peer.ask(27, dict(session_id=ready['session_id'], page_index=1), 21)


def main():
    for scenario in ('progress', 'duplicate', 'parallel', 'disconnect', 'crash',
                     'io_error', 'enospc', 'sync_error', 'install_error', 'expiry'):
        with tempfile.TemporaryDirectory(prefix='chunkharbor-materialize-') as tmp:
            root = Path(tmp) / 'backup'
            root.mkdir(mode=0o700)
            gate = Path(tmp) / 'gate'
            release = Path(str(gate) + '.release')
            entered = Path(str(gate) + '.entered')
            fault = Path(tmp) / 'fault'
            env = dict(os.environ, LD_PRELOAD=sys.argv[2], CH_PLAN_TEST_GATE=str(gate),
                       CH_MATERIALIZE_TEST_GATE=str(fault))
            server, port = wire.start(root, env)
            peers = []
            pool = concurrent.futures.ThreadPoolExecutor(max_workers=2)
            def connect():
                peer = wire.Peer(port)
                peers.append(peer)
                return peer
            try:
                _, original, _ = wire.upload(connect(), 'a', b'aaaabbbb', [[0, 2]])
                wire.upload(connect(), 'b', b'aaaabbbb', [[0, 2]])
                p = connect()
                ready = pending(p, 'a')
                observer = connect()
                observer.ask(28, dict(session_id=ready['session_id']), 29)
                if scenario == 'parallel':
                    q = connect()
                    second = pending(q, 'b')
                gate.write_text('/.versions/' if scenario == 'parallel' else original['version_id'])
                fault.write_text(ready['session_id'])
                slow = pool.submit(final_page, p, ready)
                wait_for(lambda: entered.exists(), 'materialization did not reach baseline read')
                if scenario == 'progress':
                    independent = connect()
                    independent.sock.settimeout(1)
                    started = time.monotonic()
                    new = wire.begin(independent, 'other', b'new!')
                    assert independent.ask(28, dict(session_id=new['session_id']), 29)['phase'] == 'COLLECTING'
                    assert observer.ask(28, dict(session_id=ready['session_id']), 29)['phase'] == 'PLAN_READY'
                    print('independent BEGIN/QUERY before release', time.monotonic() - started, flush=True)
                if scenario == 'duplicate':
                    assert observer.ask(27, dict(session_id=ready['session_id'], page_index=1), 15)['code'] == 'BUSY'
                if scenario == 'parallel':
                    second_future = pool.submit(final_page, q, second)
                    wait_for(lambda: entered.stat().st_size >= 2, 'different sessions serialized')
                if scenario == 'disconnect':
                    p.close()
                if scenario == 'expiry':
                    with sqlite3.connect(root / 'metadata.sqlite3') as db:
                        db.execute('UPDATE delta_sessions SET created=0 WHERE id=?', (ready['session_id'],))
                if scenario == 'install_error':
                    with sqlite3.connect(root / 'metadata.sqlite3') as db:
                        db.execute("CREATE TRIGGER reject_transfer BEFORE UPDATE OF phase ON delta_sessions "
                                   "WHEN NEW.phase=2 BEGIN SELECT RAISE(ABORT, 'test failure'); END")
                if scenario == 'crash':
                    wire.stop(server, crash=True)
                    server = None
                    try:
                        slow.result(timeout=3)
                    except (AssertionError, OSError):
                        pass
                    gate.unlink()
                    server, port = wire.start(root, env)
                    observer = connect()
                    assert observer.ask(28, dict(session_id=ready['session_id']), 29)['phase'] == 'PLAN_READY'
                    assert final_page(observer, ready)['reused_blocks'] == 1
                else:
                    if scenario in ('io_error', 'enospc', 'sync_error'):
                        marker = Path(str(gate) + '.fail') if scenario == 'io_error' else Path(str(fault) + '.' + scenario)
                        marker.touch()
                    release.touch()
                    if scenario == 'expiry':
                        assert slow.result(timeout=3)['reused_blocks'] == 1
                        assert observer.ask(28, dict(session_id=ready['session_id']), 15)['code'] == 'SESSION_NOT_FOUND'
                        reaper = wire.Peer(port)
                        peers.append(reaper)
                        wire.begin(reaper, 'reap', b'data')
                        assert not (root / '.delta' / ready['session_id']).exists()
                    elif scenario in ('io_error', 'enospc', 'sync_error', 'install_error'):
                        try:
                            slow.result(timeout=3)
                            raise AssertionError('expected materialization failure')
                        except AssertionError as error:
                            assert any(code in str(error) for code in ('INTERNAL_ERROR', 'DISK_FULL', 'SESSION_NOT_FOUND')), error
                        with sqlite3.connect(root / 'metadata.sqlite3') as db:
                            assert db.execute('SELECT sum(accepted) FROM manifest_chunks WHERE session=?',
                                              (ready['session_id'],)).fetchone()[0] == 0
                            if scenario == 'install_error':
                                db.execute('DROP TRIGGER reject_transfer')
                        if scenario != 'expiry':
                            if scenario != 'install_error':
                                marker.unlink()
                            assert observer.ask(28, dict(session_id=ready['session_id']), 29)['phase'] == 'PLAN_READY'
                            assert final_page(observer, ready)['reused_blocks'] == 1
                    else:
                        if scenario == 'disconnect':
                            try:
                                slow.result(timeout=3)
                            except (OSError, AssertionError):
                                pass
                            wait_for(lambda: observer.ask(28, dict(session_id=ready['session_id']), 29)['phase']
                                     == 'TRANSFERRING', 'disconnected materialization did not finish')
                        else:
                            assert slow.result(timeout=3)['reused_blocks'] == 1
                        if scenario == 'parallel':
                            assert second_future.result(timeout=3)['reused_blocks'] == 1
                if scenario != 'expiry':
                    # A repeated final page must never truncate already accepted data.
                    observer = connect()
                    state = observer.ask(28, dict(session_id=ready['session_id']), 29)
                    assert state['phase'] == 'TRANSFERRING' and state['missing_bitmap'] == '02'
                    wire.send_block(observer, ready['session_id'], 4, b'xxxx')
                    assert final_page(observer, ready)['reused_blocks'] == 1
                    result = observer.ask(30, dict(session_id=ready['session_id']), 31)
                    assert result['sha256'] == wire.digest(b'aaaaxxxx')
                    assert (root / 'files' / 'a').read_bytes() == b'aaaaxxxx'
                assert (root / '.versions' / original['version_id'] / 'data').read_bytes() == b'aaaabbbb'
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
