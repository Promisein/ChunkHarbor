"""Durable preparation ownership and conservative reclamation."""
import concurrent.futures
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile

import delta_protocol_test as wire
from delta_plan_lock_test import wait_for


def main():
    cases = ('rollback3', 'repeated', 'before_journal', 'journal_durable', 'object_created',
             'object_written', 'ready_rollback', 'sync_failure', 'cleanup_retry', 'expiry',
             'active_expiry', 'duplicate', 'committing', 'reference_guard', 'unknown', 'aba',
             'unexpected_child', 'journal_corrupt', 'journal_escape', 'reclaim_crash')
    for case in cases:
        with tempfile.TemporaryDirectory(prefix='chunkharbor-owner-') as tmp:
            root = Path(tmp) / 'backup'
            root.mkdir(mode=0o700)
            gate = Path(tmp) / 'gate'
            release = Path(str(gate) + '.release')
            stages = Path(tmp) / 'stage'
            marker = Path(tmp) / 'stopped'
            cleanup = Path(tmp) / 'cleanup'
            env = dict(os.environ, LD_PRELOAD=sys.argv[2], CH_COMMIT_TEST_GATE=str(gate),
                       CH_OWNER_TEST_STAGE=str(stages), CH_M4_TEST_CRASH_MARKER=str(marker),
                       CH_OWNER_CLEANUP_GATE=str(cleanup))
            server, port = wire.start(root, env)
            peers = []
            pool = concurrent.futures.ThreadPoolExecutor(max_workers=1)
            def connect():
                p = wire.Peer(port)
                peers.append(p)
                return p
            def stop():
                nonlocal server
                wire.stop(server)
                server = None
            def start():
                nonlocal server, port
                server, port = wire.start(root, env)
            def drop_faults():
                for path in (gate, stages, Path(str(gate) + '.fail'), Path(str(cleanup) + '.fail')):
                    path.unlink(missing_ok=True)
                release.touch()
            try:
                _, old, _ = wire.upload(connect(), 'a', b'aaaabbbb', [[0, 2]])
                p = connect()
                ready = wire.begin(p, 'a', b'zzzzbbbb')
                sid = ready['session_id']
                wire.manifest(p, ready, b'zzzzbbbb')
                wire.plan(p, ready, [[0, 1]])
                wire.send_block(p, sid, 0, b'zzzz')
                record = root / '.preparations' / sid
                if case == 'unknown':
                    unknown = root / '.versions' / ('e' * 32)
                    unknown.mkdir(mode=0o700)
                    (unknown / 'data').write_bytes(b'unknown')
                    (unknown / 'data').chmod(0o600)
                early = case in ('before_journal', 'journal_durable', 'object_created', 'object_written')
                if early:
                    if case == 'before_journal':
                        gate.write_text('fsync\n/.preparations/.tmp-' + sid)
                    else:
                        stages.write_text(case)
                    task = pool.submit(p.ask, 30, dict(session_id=sid), 31)
                    wait_for(lambda: (Path(str(gate) + '.entered').exists() if case == 'before_journal'
                                      else marker.exists()), 'ownership crash barrier not reached')
                    if case == 'before_journal':
                        assert not record.exists()
                        assert len(list((root / '.versions').iterdir())) == 1
                        independent = connect()
                        independent.sock.settimeout(1)
                        unrelated = wire.begin(independent, 'independent', b'data')
                        assert independent.ask(28, dict(session_id=unrelated['session_id']), 29)['phase'] == 'COLLECTING'
                    else:
                        owned = json.loads(record.read_text())
                        assert owned['session'] == sid and owned['state'] == 'preparing'
                    wire.stop(server, crash=True)
                    server = None
                    try:
                        task.result(timeout=3)
                    except (OSError, AssertionError):
                        pass
                    drop_faults()
                    start()
                elif case in ('active_expiry', 'duplicate'):
                    gate.write_text('pwrite\n/.versions/')
                    task = pool.submit(p.ask, 30, dict(session_id=sid), 31)
                    wait_for(lambda: Path(str(gate) + '.entered').exists(), 'prepare write not reached')
                    owned = json.loads(record.read_text())
                    assert owned['state'] == 'preparing'
                    if case == 'active_expiry':
                        with sqlite3.connect(root / 'metadata.sqlite3') as db:
                            db.execute('UPDATE delta_sessions SET created=0 WHERE id=?', (sid,))
                        wire.begin(connect(), 'other', b'data')
                        assert (root / '.versions' / owned['version']).exists() and record.exists()
                    else:
                        assert connect().ask(30, dict(session_id=sid), 15)['code'] == 'BUSY'
                    release.touch()
                    assert task.result(timeout=3)['committed']
                    assert not record.exists()
                else:
                    phase = 5 if case == 'committing' else 4
                    with sqlite3.connect(root / 'metadata.sqlite3') as db:
                        db.execute(f"CREATE TRIGGER fail_prepare BEFORE UPDATE OF phase ON delta_sessions "
                                   f"WHEN NEW.phase={phase} BEGIN SELECT RAISE(ABORT,'owner test'); END")
                    if case == 'sync_failure':
                        gate.write_text('fsync\n/.versions/')
                        release.touch()
                        Path(str(gate) + '.fail').touch()
                    attempts = 3 if case in ('rollback3', 'repeated') else 1
                    object_id = None
                    for attempt in range(attempts):
                        assert connect().ask(30, dict(session_id=sid), 15)['code'] == 'INTERNAL_ERROR'
                        owned = json.loads(record.read_text())
                        assert owned['session'] == sid
                        assert owned['state'] == ('preparing' if case == 'sync_failure' else 'ready')
                        assert owned['request']['metadata']['path'] == 'a'
                        if object_id is not None:
                            assert owned['version'] == object_id
                        object_id = owned['version']
                        with sqlite3.connect(root / 'metadata.sqlite3') as db:
                            state = db.execute('SELECT phase,version_id FROM delta_sessions WHERE id=?', (sid,)).fetchone()
                            assert state == ((4, object_id) if case == 'committing' else (2, None))
                        assert len(list((root / '.versions').iterdir())) == (3 if case == 'unknown' else 2)
                        if case == 'rollback3':
                            print('rollback', attempt + 1, 'session', sid, 'object', object_id,
                                  'journal', owned['state'], 'sqlite', state,
                                  'payload', (root / '.versions' / object_id / 'data').stat().st_size,
                                  flush=True)
                    if case == 'reference_guard':
                        with sqlite3.connect(root / 'metadata.sqlite3') as db:
                            db.execute('INSERT INTO versions VALUES(?,?,?,?,?)',
                                       (object_id, 'a', 8, 4, wire.digest(b'zzzzbbbb')))
                    if case in ('cleanup_retry', 'aba'):
                        cleanup.write_text(object_id)
                        Path(str(cleanup) + '.fail').touch()
                    if case == 'unexpected_child':
                        (root / '.versions' / object_id / 'foreign').write_bytes(b'keep me')
                    if case == 'expiry':
                        with sqlite3.connect(root / 'metadata.sqlite3') as db:
                            db.execute('UPDATE delta_sessions SET created=0 WHERE id=?', (sid,))
                        wire.begin(connect(), 'other', b'data')
                        assert not record.exists() and not (root / '.versions' / object_id).exists()
                        assert (root / 'files' / 'a').read_bytes() == b'aaaabbbb'
                        print(case, 'PASS', flush=True)
                        continue
                    stop()
                    if case == 'reclaim_crash':
                        stages.write_text('reclaim_removed')
                        server = subprocess.Popen([wire.SERVER, '--upload', '--root', str(root), '--port', '0'],
                                                  env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                        wait_for(marker.exists, 'reclaim removal crash barrier not reached')
                        assert record.exists() and json.loads(record.read_text())['state'] == 'reclaiming'
                        assert not (root / '.versions' / object_id).exists()
                        wire.stop(server, crash=True)
                        server = None
                        stages.unlink()
                    if case in ('journal_corrupt', 'journal_escape'):
                        original_record = record.read_bytes()
                        changed = json.loads(original_record)
                        if case == 'journal_corrupt':
                            changed['format'] = 2
                        else:
                            changed['version'] = '../outside'
                            outside = Path(tmp) / 'outside'
                            outside.write_bytes(b'keep outside')
                        record.write_text(json.dumps(changed))
                    if case == 'committing':
                        with sqlite3.connect(root / 'metadata.sqlite3') as db:
                            db.execute('DROP TRIGGER fail_prepare')
                    if case in ('cleanup_retry', 'aba', 'reference_guard', 'unexpected_child',
                                'journal_corrupt', 'journal_escape'):
                        failed = subprocess.run([wire.SERVER, '--upload', '--root', str(root), '--port', '0'],
                                                env=env, capture_output=True, text=True, timeout=5)
                        assert failed.returncode == 2 and record.exists()
                        assert (root / '.versions' / object_id).exists()
                        if case == 'reference_guard':
                            with sqlite3.connect(root / 'metadata.sqlite3') as db:
                                db.execute('DELETE FROM versions WHERE id=?', (object_id,))
                        elif case == 'unexpected_child':
                            foreign = root / '.versions' / object_id / 'foreign'
                            assert foreign.read_bytes() == b'keep me'
                            assert (foreign.parent / 'data').exists()
                            foreign.unlink()
                        elif case in ('journal_corrupt', 'journal_escape'):
                            if case == 'journal_corrupt':
                                assert json.loads(record.read_text())['format'] == 2
                            else:
                                assert outside.read_bytes() == b'keep outside'
                            record.write_bytes(original_record)
                        else:
                            assert json.loads(record.read_text())['state'] == 'reclaiming'
                            Path(str(cleanup) + '.fail').unlink()
                    drop_faults()
                    start()
                    if case != 'committing':
                        assert not (root / '.versions' / object_id).exists() and not record.exists()
                    if case == 'unknown':
                        assert (root / '.quarantine' / ('version-' + 'e' * 32) / 'data').read_bytes() == b'unknown'
                    if case == 'repeated':
                        for cycle in range(3):
                            assert connect().ask(30, dict(session_id=sid), 15)['code'] == 'INTERNAL_ERROR'
                            stop()
                            start()
                            assert len(list((root / '.versions').iterdir())) == 1
                            assert not list((root / '.preparations').iterdir())
                    with sqlite3.connect(root / 'metadata.sqlite3') as db:
                        db.execute('DROP TRIGGER IF EXISTS fail_prepare')
                if case not in ('active_expiry', 'duplicate'):
                    # Accepted upload data survives preparation reclamation.
                    status = connect().ask(28, dict(session_id=sid), 29)
                    assert status['phase'] == 'COMMITTED' or status['missing_bitmap'] == '00'
                    result = connect().ask(30, dict(session_id=sid), 31)
                    assert result['committed'] and result['sha256'] == wire.digest(b'zzzzbbbb')
                    if case == 'aba':
                        assert result['version_id'] != object_id
                assert (root / '.versions' / old['version_id'] / 'data').read_bytes() == b'aaaabbbb'
                assert (root / 'files' / 'a').read_bytes() == b'zzzzbbbb'
                assert not record.exists()
                for restart in range(3):
                    stop()
                    start()
                    assert connect().ask(28, dict(session_id=sid), 29)['phase'] == 'COMMITTED'
                    if case == 'unknown':
                        assert (root / '.quarantine' / ('version-' + 'e' * 32) / 'data').read_bytes() == b'unknown'
                with sqlite3.connect(root / 'metadata.sqlite3') as db:
                    assert db.execute('PRAGMA integrity_check').fetchone()[0] == 'ok'
                    assert db.execute('PRAGMA foreign_key_check').fetchall() == []
                print(case, 'PASS', flush=True)
            finally:
                release.touch()
                for p in peers:
                    p.close()
                if server is not None:
                    wire.stop(server)
                pool.shutdown(wait=True)


if __name__ == '__main__':
    main()
