"""M4 wire/storage integration. All data belongs to a fresh temporary root."""
import hashlib
import json
import os
from pathlib import Path
import queue
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading

SERVER = sys.argv[1]
HEADER = struct.Struct('!IHHIQQI')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def exact(sock, length):
    result = bytearray()
    while len(result) < length:
        data = sock.recv(length - len(result))
        assert data, 'unexpected disconnect'
        result.extend(data)
    return bytes(result)


class Peer:
    def __init__(self, port, delta=True):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=30)
        self.request_id = 0
        caps = ['handshake', 'upload'] + (['fixed_block_delta_v1'] if delta else [])
        ack = self.ask(1, {'client_name': 'm4-wire-test', 'versions': [1], 'capabilities': caps}, 2)
        if delta:
            assert 'fixed_block_delta_v1' in ack['capabilities'], ack

    def close(self):
        self.sock.close()

    def ask(self, kind, value, expected):
        self.request_id += 1
        body = value if isinstance(value, bytes) else json.dumps(value, separators=(',', ':')).encode()
        self.sock.sendall(HEADER.pack(0x43484B48, 1, kind, 0, self.request_id, len(body), 0) + body)
        magic, version, typ, flags, rid, length, reserved = HEADER.unpack(exact(self.sock, 32))
        assert (magic, version, flags, rid, reserved) == (0x43484B48, 1, 0, self.request_id, 0)
        assert length <= 65536
        reply = json.loads(exact(self.sock, length))
        assert typ == expected, (kind, typ, expected, reply)
        return reply


def start(root, env=None):
    p = subprocess.Popen([SERVER, '--upload', '--root', str(root), '--port', '0'],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    lines = queue.Queue()
    threading.Thread(target=lambda: lines.put(p.stdout.readline()), daemon=True).start()
    try:
        line = lines.get(timeout=15).strip()
    except queue.Empty:
        p.kill()
        raise AssertionError('server startup timed out')
    assert line.startswith('LISTENING 127.0.0.1:'), (line, p.stderr.read())
    return p, int(line.rsplit(':', 1)[1])


def stop(p, crash=False):
    p.kill() if crash else p.terminate()
    p.communicate(timeout=15)


def begin(peer, path, content, chunk_size=4, mode='upload', base='auto'):
    meta = {'path': path, 'size': len(content), 'mtime_ns': 9007199254740993,
            'chunk_size': chunk_size, 'sha256': digest(content)}
    return peer.ask(24, {'metadata': meta, 'mode': mode, 'base_version': base}, 25)


def manifest(peer, ready, content, chunk_size=4, start_page=0):
    hashes = [digest(content[i:i + chunk_size]) for i in range(0, len(content), chunk_size)]
    for page, start_index in enumerate(range(0, len(hashes), 512)):
        if page < start_page:
            continue
        value = {'session_id': ready['session_id'], 'page_index': page,
                 'first_block_index': start_index, 'hashes': hashes[start_index:start_index + 512]}
        ack = peer.ask(18, value, 26)
        assert ack['next_page_index'] == page + 1
    ack = peer.ask(19, {'session_id': ready['session_id'], 'page_count': (len(hashes) + 511) // 512,
                         'block_count': len(hashes)}, 26)
    assert ack['phase'] == 'PLAN_READY'


def plan(peer, ready, expected_ranges):
    ranges = []
    pages = (len(expected_ranges) + 511) // 512
    for index in range(pages):
        reply = peer.ask(27, {'session_id': ready['session_id'], 'page_index': index}, 20)
        assert reply['base_version'] == ready['base_version'] and reply['page_index'] == index
        assert 0 < len(reply['missing_ranges']) <= 512
        ranges.extend(reply['missing_ranges'])
    done = peer.ask(27, {'session_id': ready['session_id'], 'page_index': pages}, 21)
    assert ranges == expected_ranges, (ranges, expected_ranges)
    assert done['missing_blocks'] == sum(end - first for first, end in ranges)
    return ranges, done


def send_block(peer, sid, offset, data):
    body = bytes.fromhex(sid) + struct.pack('!QI', offset, len(data)) + hashlib.sha256(data).digest() + data
    ack = peer.ask(5, body, 6)
    assert ack['durable'] is True and ack['offset'] == offset


def upload(peer, path, content, expected_ranges, chunk_size=4):
    ready = begin(peer, path, content, chunk_size)
    manifest(peer, ready, content, chunk_size)
    ranges, done = plan(peer, ready, expected_ranges)
    for first, end in ranges:
        for index in range(first, end):
            send_block(peer, ready['session_id'], index * chunk_size,
                       content[index * chunk_size:(index + 1) * chunk_size])
    result = peer.ask(30, {'session_id': ready['session_id']}, 31)
    assert result['committed'] and result['sha256'] == digest(content)
    return ready, result, done


def main():
    with tempfile.TemporaryDirectory(prefix='chunkharbor-m4-wire-') as tmp:
        root = Path(tmp) / 'backup'
        root.mkdir(mode=0o700)
        server, port = start(root)
        try:
            old = Peer(port, delta=False)
            try:
                err = old.ask(24, {}, 15)
                assert err['code'] == 'UNSUPPORTED_OPERATION'
            finally:
                old.close()
            peer = Peer(port)
            _, first, _ = upload(peer, 'dir/文件.bin', b'abcdefghij', [[0, 3]])
            peer.close()
            peer = Peer(port)
            _, same, stats = upload(peer, 'dir/文件.bin', b'abcdefghij', [])
            assert same['version_id'] == first['version_id'] and stats['missing_bytes'] == 0
            peer.close()
            peer = Peer(port)
            _, changed, stats = upload(peer, 'dir/文件.bin', b'abcdWXYZij', [[1, 2]])
            assert changed['version_id'] != first['version_id'] and stats['missing_bytes'] == 4
            peer.close()
            # Explicit stale baselines must fail even after a different valid version exists.
            peer = Peer(port)
            meta = {'path': 'dir/文件.bin', 'size': 10, 'mtime_ns': 0, 'chunk_size': 4,
                    'sha256': digest(b'abcdWXYZij')}
            error = peer.ask(24, {'metadata': meta, 'mode': 'upload',
                                  'base_version': first['version_id']}, 15)
            assert error['code'] == 'CONFLICT'
            peer.close()
            assert (root / 'files' / 'dir' / '文件.bin').read_bytes() == b'abcdWXYZij'
            # Preview is read-only with regard to actual backup contents/current version.
            peer = Peer(port)
            preview = begin(peer, 'dir/文件.bin', b'abcdWXYZij!', mode='preview')
            manifest(peer, preview, b'abcdWXYZij!')
            _, preview_stats = plan(peer, preview, [[2, 3]])
            assert preview_stats['missing_bytes'] == 3
            assert preview_stats['change_kind'] == 'Modified'
            assert (root / 'files' / 'dir' / '文件.bin').read_bytes() == b'abcdWXYZij'
            err = peer.ask(30, {'session_id': preview['session_id']}, 15)
            assert err['code'] in ('CONFLICT', 'MALFORMED_MESSAGE', 'UNSUPPORTED_OPERATION')
            peer.close()
            # Shortening and empty files must never retain old trailing data.
            peer = Peer(port)
            upload(peer, 'dir/文件.bin', b'abcd', [])
            peer.close()
            assert (root / 'files' / 'dir' / '文件.bin').read_bytes() == b'abcd'
            peer = Peer(port)
            upload(peer, 'dir/文件.bin', b'', [])
            peer.close()
            assert (root / 'files' / 'dir' / '文件.bin').read_bytes() == b''
            # Synced bytes without a successful metadata update must never earn an ACK.
            peer = Peer(port)
            failed = begin(peer, 'db-fault.bin', b'data')
            manifest(peer, failed, b'data')
            plan(peer, failed, [[0, 1]])
            with sqlite3.connect(root / 'metadata.sqlite3') as db:
                db.execute("CREATE TRIGGER reject_delta_confirmation BEFORE UPDATE OF accepted ON manifest_chunks "
                           "WHEN NEW.accepted=1 BEGIN SELECT RAISE(ABORT,'test failure'); END")
            body = bytes.fromhex(failed['session_id']) + struct.pack('!QI', 0, 4) + hashlib.sha256(b'data').digest() + b'data'
            assert peer.ask(5, body, 15)['code'] == 'INTERNAL_ERROR'
            peer.close()
            with sqlite3.connect(root / 'metadata.sqlite3') as db:
                db.execute('DROP TRIGGER reject_delta_confirmation')
            peer = Peer(port)
            assert peer.ask(28, {'session_id': failed['session_id']}, 29)['missing_bitmap'] == '01'
            send_block(peer, failed['session_id'], 0, b'data')
            peer.ask(30, {'session_id': failed['session_id']}, 31)
            peer.close()
            # Persistent partial manifest/data survives a real process SIGKILL.
            content = b'restart-test-data'
            peer = Peer(port)
            ready = begin(peer, 'restart.bin', content)
            manifest(peer, ready, content)
            plan(peer, ready, [[0, 5]])
            send_block(peer, ready['session_id'], 0, content[:4])
            peer.close()
        finally:
            stop(server, crash=True)
        server, port = start(root)
        try:
            peer = Peer(port)
            status = peer.ask(28, {'session_id': ready['session_id']}, 29)
            missing = bytes.fromhex(status['missing_bitmap'])
            assert missing == bytes([0b11110]), status
            for index in range(1, 5):
                send_block(peer, ready['session_id'], index * 4, content[index * 4:(index + 1) * 4])
            result = peer.ask(30, {'session_id': ready['session_id']}, 31)
            assert result['sha256'] == digest(content)
            assert peer.ask(30, {'session_id': ready['session_id']}, 31) == result
            peer.close()
            assert (root / 'files' / 'restart.bin').read_bytes() == content
            # Reusing an unchanged version must also preserve its immutable chunk geometry.
            peer = Peer(port)
            _, geometry, _ = upload(peer, 'geometry.bin', b'abcdef', [[0, 2]], chunk_size=3)
            peer.close()
            with sqlite3.connect(root / 'metadata.sqlite3') as db:
                before = db.execute('SELECT idx,sha256,length FROM version_chunks WHERE version_id=? ORDER BY idx',
                                    (geometry['version_id'],)).fetchall()
            peer = Peer(port)
            _, unchanged, _ = upload(peer, 'geometry.bin', b'abcdef', [[0, 3]], chunk_size=2)
            peer.close()
            assert unchanged['version_id'] == geometry['version_id']
            with sqlite3.connect(root / 'metadata.sqlite3') as db:
                after = db.execute('SELECT idx,sha256,length FROM version_chunks WHERE version_id=? ORDER BY idx',
                                   (geometry['version_id'],)).fetchall()
            assert after == before, ('immutable version chunk geometry changed', before, after)
            # More than one manifest AND plan page: 1024 alternating missing ranges.
            baseline = b'a' * 8192
            peer = Peer(port)
            upload(peer, 'paged.bin', baseline, [[0, 2048]])
            peer.close()
            modified = b'bbbb' + b'aaaa'
            peer = Peer(port)
            upload(peer, 'paged.bin', modified * 1024, [[i, i + 1] for i in range(0, 2048, 2)])
            peer.close()
            assert (root / 'files' / 'paged.bin').read_bytes() == modified * 1024
        finally:
            stop(server)
    print('M4 protocol, versions, preview, paging and SIGKILL resume passed')


if __name__ == '__main__':
    main()
