"""Controlled storage errors: never ACK failed persistence; retry private data."""
import os
from pathlib import Path
import sys
import tempfile
import delta_protocol_test as wire

SERVER, HOOK = sys.argv[1:3]
for stage in ('data_write', 'data_sync', 'object_sync', 'publish_sync'):
    with tempfile.TemporaryDirectory(prefix='chunkharbor-m4-fault-') as temp:
        root = Path(temp) / 'backup'
        root.mkdir(mode=0o700)
        process, port = wire.start(root)
        peer = wire.Peer(port)
        wire.upload(peer, 'file', b'abcdefgh', [[0, 2]])
        peer.close()
        wire.stop(process)
        env = dict(os.environ, LD_PRELOAD=str(Path(HOOK).resolve()), CH_M4_TEST_FAIL_STAGE=stage)
        process, port = wire.start(root, env=env)
        peer = wire.Peer(port)
        ready = wire.begin(peer, 'file', b'WXYZ1234')
        wire.manifest(peer, ready, b'WXYZ1234')
        wire.plan(peer, ready, [[0, 2]])
        if stage.startswith('data_'):
            data = b'WXYZ'
            import struct
            import hashlib
            body = bytes.fromhex(ready['session_id']) + struct.pack('!QI', 0, 4) + hashlib.sha256(data).digest() + data
            error = peer.ask(5, body, 15)
            assert error['code'] == ('DISK_FULL' if stage == 'data_write' else 'INTERNAL_ERROR'), error
        else:
            wire.send_block(peer, ready['session_id'], 0, b'WXYZ')
            wire.send_block(peer, ready['session_id'], 4, b'1234')
            assert peer.ask(30, {'session_id': ready['session_id']}, 15)['code'] == 'INTERNAL_ERROR'
        peer.close()
        wire.stop(process)
        process, port = wire.start(root)
        try:
            peer = wire.Peer(port)
            status = peer.ask(28, {'session_id': ready['session_id']}, 29)
            if stage.startswith('data_'):
                assert status['missing_bitmap'] == '03', status
                wire.send_block(peer, ready['session_id'], 0, b'WXYZ')
                wire.send_block(peer, ready['session_id'], 4, b'1234')
            elif stage == 'object_sync':
                assert status['phase'] == 'TRANSFERRING', status
            else:
                assert status['phase'] == 'COMMITTED', status
            result = peer.ask(30, {'session_id': ready['session_id']}, 31)
            assert result['sha256'] == wire.digest(b'WXYZ1234')
            assert (root / 'files' / 'file').read_bytes() == b'WXYZ1234'
            peer.close()
        finally:
            wire.stop(process)
print('M4 ENOSPC, data sync, object sync and publish sync injection passed')
