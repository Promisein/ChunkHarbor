"""In-flight durable work is not an idle peer; only private temporary data."""
import hashlib
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import delta_protocol_test as wire

SERVER, HOOK = sys.argv[1:3]


def delayed_block(peer, ready, marker):
    peer.request_id += 1
    body = bytes.fromhex(ready['session_id']) + struct.pack('!QI', 0, 4) + hashlib.sha256(b'abcd').digest() + b'abcd'
    peer.sock.sendall(wire.HEADER.pack(0x43484B48, 1, 5, 0, peer.request_id, len(body), 0) + body)
    until = time.monotonic() + 5
    while not marker.exists():
        assert time.monotonic() < until, 'slow fdatasync hook was not reached'
        time.sleep(.01)


def receive_ack(peer):
    import json
    magic, version, kind, flags, rid, length, reserved = wire.HEADER.unpack(wire.exact(peer.sock, 32))
    assert (magic, version, kind, flags, rid, reserved) == (0x43484B48, 1, 6, 0, peer.request_id, 0)
    ack = json.loads(wire.exact(peer.sock, length))
    assert ack['durable'] and ack['offset'] == 0 and ack['data_length'] == 4, ack


with tempfile.TemporaryDirectory(prefix='chunkharbor-upload-idle-') as temp:
    root = Path(temp) / 'backup'
    root.mkdir(mode=0o700)
    marker = Path(temp) / 'sync-entered'
    env = dict(os.environ, LD_PRELOAD=str(Path(HOOK).resolve()), CH_UPLOAD_DELAY_MARKER=str(marker))
    process = subprocess.Popen([SERVER, '--upload', '--root', str(root), '--port', '0',
                                '--idle-timeout-ms', '500'], stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True, env=env)
    peer = None
    try:
        line = process.stdout.readline().strip()
        assert line.startswith('LISTENING 127.0.0.1:'), line
        port = int(line.rsplit(':', 1)[1])
        peer = wire.Peer(port)
        ready = wire.begin(peer, 'slow', b'abcdefgh')
        wire.manifest(peer, ready, b'abcdefgh')
        wire.plan(peer, ready, [[0, 2]])
        delayed_block(peer, ready, marker)
        # Beyond idle deadline, before real fdatasync: no early ACK and no EOF.
        peer.sock.settimeout(.9)
        try:
            received = peer.sock.recv(1, socket.MSG_PEEK)
            raise AssertionError(f'pending disk work was closed or acknowledged early: {received!r}')
        except socket.timeout:
            pass
        peer.sock.settimeout(5)
        receive_ack(peer)
        wire.send_block(peer, ready['session_id'], 4, b'efgh')
        done = peer.ask(30, {'session_id': ready['session_id']}, 31)
        assert done['committed'] and (root / 'files/slow').read_bytes() == b'abcdefgh'
        peer.close()
        # Real idle peers must still be reaped, then reconnect successfully.
        peer = wire.Peer(port)
        time.sleep(.9)
        assert peer.sock.recv(1) == b'', 'idle timeout was accidentally disabled'
        peer.close()
        peer = wire.Peer(port)
        wire.upload(peer, 'normal', b'abcdefgh', [[0, 2]])
        peer.close()
        # Lose a connection/ACK during durable work, query authoritative missing
        # bits on a fresh connection, send only the missing second block, commit.
        peer = wire.Peer(port)
        ready = wire.begin(peer, 'retry', b'abcdefgh')
        wire.manifest(peer, ready, b'abcdefgh')
        wire.plan(peer, ready, [[0, 2]])
        marker.unlink()
        delayed_block(peer, ready, marker)
        peer.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        peer.close()
        time.sleep(2.2)
        peer = wire.Peer(port)
        status = peer.ask(28, {'session_id': ready['session_id']}, 29)
        assert status['missing_bitmap'] == '02', status
        wire.send_block(peer, ready['session_id'], 4, b'efgh')
        result = peer.ask(30, {'session_id': ready['session_id']}, 31)
        assert result['committed'] and (root / 'files/retry').read_bytes() == b'abcdefgh'
        print('slow durable ACK, real idle close, normal upload and lost-ACK reconnect/resume passed')
    finally:
        if peer:
            peer.close()
        wire.stop(process)
