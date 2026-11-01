"""Stop at native publication barriers, SIGKILL, then recover a private root."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import delta_protocol_test as wire

SERVER, HOOK = sys.argv[1:3]


def main():
    for stage in ('object_synced', 'before_latest_rename', 'after_latest_rename', 'after_committed_cleanup'):
        with tempfile.TemporaryDirectory(prefix='chunkharbor-m4-crash-') as tmp:
            root = Path(tmp) / 'backup'
            root.mkdir(mode=0o700)
            server, port = wire.start(root)
            peer = wire.Peer(port)
            wire.upload(peer, 'crash.bin', b'abcdefgh', [[0, 2]])
            peer.close()
            wire.stop(server)
            marker = Path(tmp) / 'stopped'
            env = dict(os.environ, LD_PRELOAD=str(Path(HOOK).resolve()),
                       CH_M4_TEST_CRASH_STAGE=stage, CH_M4_TEST_CRASH_MARKER=str(marker))
            server, port = wire.start(root, env=env)
            peer = wire.Peer(port)
            ready = wire.begin(peer, 'crash.bin', b'abcdWXYZ')
            wire.manifest(peer, ready, b'abcdWXYZ')
            wire.plan(peer, ready, [[1, 2]])
            wire.send_block(peer, ready['session_id'], 4, b'WXYZ')
            errors = []

            def commit():
                try:
                    peer.ask(30, {'session_id': ready['session_id']}, 31)
                except (OSError, AssertionError) as error:
                    errors.append(str(error))

            thread = threading.Thread(target=commit, daemon=True)
            thread.start()
            deadline = time.monotonic() + 15
            while not marker.exists() and time.monotonic() < deadline:
                if server.poll() is not None:
                    break
                time.sleep(.01)
            if not marker.exists():
                wire.stop(server, crash=True)
                raise AssertionError('publication barrier never reached: ' + stage)
            wire.stop(server, crash=True)
            thread.join(timeout=5)
            peer.close()
            server, port = wire.start(root)
            try:
                peer = wire.Peer(port)
                status = peer.ask(28, {'session_id': ready['session_id']}, 29)
                if status['phase'] != 'COMMITTED':
                    assert stage == 'object_synced' and status['phase'] == 'TRANSFERRING', status
                result = peer.ask(30, {'session_id': ready['session_id']}, 31)
                assert result['sha256'] == wire.digest(b'abcdWXYZ')
                assert (root / 'files' / 'crash.bin').read_bytes() == b'abcdWXYZ'
                assert peer.ask(30, {'session_id': ready['session_id']}, 31) == result
                peer.close()
            finally:
                wire.stop(server)
    print('M4 publication barriers: four SIGKILL recoveries passed')


if __name__ == '__main__':
    main()
