"""Read every preserved v2 object after migration and exercise historical access."""
import hashlib
import json
import os
import socket
import subprocess
from pathlib import Path
import sqlite3
import struct
import sys
import tarfile
import tempfile
import time

import delta_protocol_test as wire
from version_history_test import peer, page

CAP = 'historical_download_v1'
PATH = 'fixture/file.txt'


def start_history(port):
    p = wire.Peer.__new__(wire.Peer)
    p.sock = socket.create_connection(('127.0.0.1', port), timeout=10)
    p.request_id = 0
    reply = p.ask(1, dict(client_name='restore-test', versions=[1], capabilities=[CAP]), 2)
    assert CAP in reply['capabilities'], reply
    return p


def pull(p, ready):
    out = bytearray()
    while len(out) < ready['size']:
        offset = len(out)
        length = min(262144, ready['size'] - offset)
        data = json.dumps(dict(version_id=ready['version_id'], offset=offset, length=length)).encode()
        p.request_id += 1
        p.sock.sendall(wire.HEADER.pack(0x43484B48, 1, 33, 0, p.request_id, len(data), 0) + data)
        magic, version, kind, flags, rid, size, reserved = wire.HEADER.unpack(wire.exact(p.sock, 32))
        assert (magic, version, kind, flags, rid, size, reserved) == (
            0x43484B48, 1, 14, 0, p.request_id, length + 60, 0)
        body = wire.exact(p.sock, size)
        assert body[:16].hex() == ready['version_id']
        assert struct.unpack('!QI', body[16:28]) == (offset, length)
        assert body[28:60] == hashlib.sha256(body[60:]).digest()
        out.extend(body[60:])
    assert wire.digest(out) == ready['sha256']
    return bytes(out)


def restore(port, path, vid, expected):
    p = start_history(port)
    try:
        ready = p.ask(36, dict(path=path, version_id=vid), 32)
        assert ready['path'] == path and ready['version_id'] == vid
        assert pull(p, ready) == expected
    finally:
        p.close()


def error(port, path, vid, code):
    p = start_history(port)
    try:
        assert p.ask(36, dict(path=path, version_id=vid), 15)['code'] == code
    finally:
        p.close()


def main():
    with tempfile.TemporaryDirectory(prefix='chunkharbor-r23-') as tmp:
        with tarfile.open(sys.argv[3]) as archive:
            archive.extractall(tmp, filter='data')
        root = Path(tmp) / 'storage'
        root.chmod(0o700)
        for item in root.rglob('*'):
            item.chmod(0o700 if item.is_dir() else 0o600)
        with sqlite3.connect(root / 'metadata.sqlite3') as db:
            assert db.execute('PRAGMA user_version').fetchone() == (2,)
            old_pointer = db.execute('SELECT version_id FROM current_files WHERE path=?', (PATH,)).fetchone()
        server, port = wire.start(root)
        try:
            probe = start_history(port)
            probe.close()
            h = peer(port, ['version_history_v1'])
            rows = page(h, PATH)['versions']
            assert len(rows) == 3 and rows[0]['version_id'] == old_pointer[0]
            for row, suffix in zip(reversed(rows), b'ABC'):
                restore(port, PATH, row['version_id'], b'R2.2 fixture ' + bytes([suffix]))
            with sqlite3.connect(root / 'metadata.sqlite3') as db:
                assert db.execute('SELECT version_id FROM current_files WHERE path=?', (PATH,)).fetchone() == old_pointer
            legacy = page(h, 'fixture/legacy.txt')['versions'][0]
            restore(port, 'fixture/legacy.txt', legacy['version_id'], b'legacy fixture')
            source = Path(tmp) / 'fresh-legacy'
            source.write_bytes(b'fresh legacy')
            subprocess.run([sys.argv[2], 'upload', '--port', str(port), '--file', str(source),
                            '--path', 'fixture/fresh-legacy'], check=True, capture_output=True)
            fresh = page(h, 'fixture/fresh-legacy')['versions'][0]
            with sqlite3.connect(root / 'metadata.sqlite3') as db:
                assert not db.execute('SELECT 1 FROM versions WHERE id=?', (fresh['version_id'],)).fetchone()
            restore(port, 'fixture/fresh-legacy', fresh['version_id'], b'fresh legacy')
            older = rows[-1]['version_id']
            pinned = start_history(port)
            ready = pinned.ask(36, dict(path=PATH, version_id=older), 32)
            up = wire.Peer(port)
            wire.upload(up, PATH, b'DDDD', [[0, 1]])
            up.close()
            assert pull(pinned, ready) == b'R2.2 fixture A'
            pinned.close()
            restore(port, PATH, older, b'R2.2 fixture A')
            legacy_peer = peer(port, ['download_v1'])
            assert legacy_peer.ask(13, dict(path=PATH, version_id=older), 15)['code'] == 'VERSION_CONFLICT'
            legacy_peer.close()
            denied = peer(port, ['download_v1'])
            assert denied.ask(36, dict(path=PATH, version_id=older), 15)['code'] == 'UNSUPPORTED_OPERATION'
            denied.close()
            for path, vid, code in [(PATH, 'ff' * 16, 'VERSION_NOT_FOUND'),
                                    ('fixture/legacy.txt', older, 'VERSION_NOT_FOUND'),
                                    (PATH, None, 'MALFORMED_MESSAGE'),
                                    ('../x', older, 'MALFORMED_MESSAGE')]:
                error(port, path, vid, code)
            for content, path in [(b'', '空文件'), (bytes(range(256)) * 3101, '中文/多块.bin')]:
                up = wire.Peer(port)
                size = 262144
                blocks = (len(content) + size - 1) // size
                _, original, _ = wire.upload(up, path, content, [[0, blocks]] if blocks else [], size)
                up.close()
                up = wire.Peer(port)
                wire.upload(up, path, b'new', [[0, 1]], size)
                up.close()
                restore(port, path, original['version_id'], content)
            obj = root / '.versions' / older / 'data'
            moved = obj.with_name('test-held')
            obj.rename(moved)
            error(port, PATH, older, 'VERSION_UNAVAILABLE')
            obj.symlink_to(moved)
            error(port, PATH, older, 'VERSION_UNAVAILABLE')
            obj.unlink()
            moved.rename(obj)
            obj.write_bytes(b'Z' * len(b'R2.2 fixture A'))
            error(port, PATH, older, 'CHECKSUM_MISMATCH')
            h.close()
        finally:
            wire.stop(server)
    print('migrated A/B/C, legacy, pinned version, UTF-8/empty/multichunk and failure matrix passed')


def publication(hook):
    for crash in (False, True):
        with tempfile.TemporaryDirectory(prefix='chunkharbor-legacy-publish-') as directory:
            base = Path(directory)
            root = base / 'storage'
            root.mkdir(mode=0o700)
            source = base / 'source'
            data = b'legacy durable version'
            source.write_bytes(data)
            marker, release = base / 'blocked', base / 'release'
            server, port = wire.start(root, dict(os.environ, LD_PRELOAD=hook,
                CH_LEGACY_VERSION_SYNC_MARKER=str(marker), CH_LEGACY_VERSION_SYNC_RELEASE=str(release)))
            upload = subprocess.Popen([sys.argv[2], 'upload', '--file', str(source), '--path', PATH,
                                       '--port', str(port)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                deadline = time.monotonic() + 8
                while not marker.exists():
                    assert upload.poll() is None and time.monotonic() < deadline, 'legacy fsync barrier absent'
                    time.sleep(.01)
                with sqlite3.connect(root / 'metadata.sqlite3') as db:
                    assert db.execute('SELECT state FROM transfers').fetchone() == ('COMMITTING',)
                h = peer(port, ['version_history_v1'])
                assert page(h, PATH)['versions'] == []
                h.close()
                if crash:
                    wire.stop(server, crash=True)
                    assert upload.communicate(timeout=10) and upload.returncode != 0
                    server, port = wire.start(root)
                else:
                    release.touch()
                    stdout, stderr = upload.communicate(timeout=10)
                    assert upload.returncode == 0, (stdout, stderr)
                h = peer(port, ['version_history_v1'])
                rows = page(h, PATH)['versions']
                assert len(rows) == 1 and rows[0]['current']
                h.close()
                restore(port, PATH, rows[0]['version_id'], data)
            finally:
                release.touch(exist_ok=True)
                if upload.poll() is None:
                    upload.terminate()
                    upload.communicate(timeout=10)
                if server.poll() is None:
                    wire.stop(server)
    print('legacy history becomes visible only after the object is durable, including restart recovery')


if __name__ == '__main__':
    if sys.argv[3] == '--publication':
        publication(sys.argv[4])
    else:
        main()
