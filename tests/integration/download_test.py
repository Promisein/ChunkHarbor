"""Linux download loopback; all source, backup and destination data are private test data."""
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import delta_protocol_test as w

SERVER, CLI = sys.argv[1:3]


class Peer(w.Peer):
    def __init__(self, port, caps=None):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=20)
        self.request_id = 0
        ack = self.ask(1, {'client_name': 'download-test', 'versions': [1],
                          'capabilities': caps or ['download_v1']}, 2)
        self.caps = ack['capabilities']

    def chunk(self, version, offset, length):
        self.request_id += 1
        body = json.dumps(dict(version_id=version, offset=offset, length=length)).encode()
        self.sock.sendall(w.HEADER.pack(0x43484B48, 1, 33, 0, self.request_id, len(body), 0) + body)
        magic, ver, kind, flags, rid, count, reserved = w.HEADER.unpack(w.exact(self.sock, 32))
        assert (magic, ver, kind, flags, rid, reserved) == (0x43484B48, 1, 14, 0, self.request_id, 0)
        assert count <= 262144 + 60
        data = w.exact(self.sock, count)
        assert data[:16].hex() == version
        assert struct.unpack('!QI', data[16:28]) == (offset, length)
        assert data[28:60] == hashlib.sha256(data[60:]).digest()
        return data[60:]


def cli(port, logical, output, *extra, ok=True):
    r = subprocess.run([CLI, 'download', '--port', str(port), '--path', logical,
                        '--output', str(output), *extra], capture_output=True, timeout=20)
    assert (r.returncode == 0) == ok, (r.returncode, r.stdout, r.stderr)
    assert (b'DOWNLOAD OK' in r.stdout) == ok
    return r


def main():
    with tempfile.TemporaryDirectory(prefix='chunkharbor-download-') as temp:
        base = Path(temp)
        root = base / 'data'
        root.mkdir(mode=0o700)
        out = base / 'restored'
        out.mkdir()
        server, port = w.start(root)
        try:
            p = Peer(port)
            assert 'download_v1' in p.caps, p.caps
            p.close()
            contents = {'empty': b'', '中文.bin': bytes(range(256)) * 2500, 'small': b'hello restore'}
            for name, content in contents.items():
                source = base / 'source'
                source.write_bytes(content)
                # Legacy upload must download immediately without waiting for a migration.
                subprocess.run([CLI, 'upload', '--port', str(port), '--file', str(source),
                                '--path', name], capture_output=True, check=True)
                cli(port, name, out / name)
                assert (out / name).read_bytes() == content
            cli(port, 'small', out / 'small', ok=False)
            assert (out / 'small').read_bytes() == contents['small']
            sentinel = out / 'sentinel'
            sentinel.write_bytes(b'keep')
            (out / 'link').symlink_to(sentinel)
            cli(port, 'small', out / 'link', ok=False)
            assert sentinel.read_bytes() == b'keep'
            (out / 'linkdir').symlink_to(base, target_is_directory=True)
            cli(port, 'small', out / 'linkdir' / 'escaped', ok=False)
            assert not (base / 'escaped').exists()
            cli(port, '../bad', out / 'bad', ok=False)
            cli(port, 'missing', out / 'missing', ok=False)
            cli(port, 'small', out / 'wrong-version', '--version-id', '0' * 32, ok=False)
            # A pinned version remains readable after publication of a new current version.
            up = w.Peer(port)
            _, old, _ = w.upload(up, 'delta', b'first', [[0, 2]])
            up.close()
            p = Peer(port)
            ready = p.ask(13, {'path': 'delta', 'version_id': old['version_id']}, 32)
            up = w.Peer(port)
            _, current, _ = w.upload(up, 'delta', b'second', [[0, 2]])
            up.close()
            assert p.chunk(ready['version_id'], 0, 5) == b'first'
            p.close()
            cli(port, 'delta', out / 'new-delta')
            assert (out / 'new-delta').read_bytes() == b'second'
            # A corrupt committed object must fail verification before READY.
            stored = root / '.versions' / current['version_id'] / 'data'
            stored.write_bytes(b'broken')
            r = cli(port, 'delta', out / 'corrupt', ok=False)
            assert b'CHECKSUM_MISMATCH' in r.stderr
            stored.write_bytes(b'second')
            # Replacing an object with a symlink or FIFO must neither escape nor block.
            saved = stored.with_name('saved-data')
            victim = base / 'victim'
            victim.write_bytes(b'outside')
            stored.rename(saved)
            stored.symlink_to(victim)
            cli(port, 'delta', out / 'server-link', ok=False)
            assert victim.read_bytes() == b'outside'
            stored.unlink()
            os.mkfifo(stored, 0o600)
            cli(port, 'delta', out / 'server-fifo', ok=False)
            stored.unlink()
            saved.rename(stored)
            pending = w.Peer(port)
            w.begin(pending, 'uncommitted', b'x')
            pending.close()
            cli(port, 'uncommitted', out / 'uncommitted', ok=False)
            for value in [{'path': '../x', 'version_id': None}, {'path': 'small', 'version_id': 'BAD'}]:
                p = Peer(port)
                assert p.ask(13, value, 15)['code'] == 'MALFORMED_MESSAGE'
                p.close()
            p = Peer(port, ['handshake'])
            assert p.ask(13, {'path': 'small', 'version_id': None}, 15)['code'] == 'UNSUPPORTED_OPERATION'
            p.close()
            p = Peer(port)
            ready = p.ask(13, {'path': 'small', 'version_id': None}, 32)
            assert p.ask(33, {'version_id': ready['version_id'], 'offset': 999, 'length': 1}, 15)['code'] == 'INVALID_RANGE'
            p.close()
            p = Peer(port)
            assert p.ask(33, {'version_id': '0' * 32, 'offset': 0, 'length': 1}, 15)['code'] == 'SESSION_NOT_FOUND'
            p.close()
            p = Peer(port)
            p.ask(13, {'path': 'empty', 'version_id': None}, 32)
            assert p.ask(13, {'path': 'small', 'version_id': None}, 15)['code'] == 'CONFLICT'
            p.close()
            assert sorted(p.name for p in out.iterdir() if p.name.startswith('.chunkharbor-')) == []
        finally:
            w.stop(server)
        server, port = w.start(root)
        try:
            cli(port, 'small', out / 'after-restart')
            assert (out / 'after-restart').read_bytes() == contents['small']
        finally:
            w.stop(server)
    print('Download empty/binary/legacy/delta/version pinning/safe publication/restart passed')


if __name__ == '__main__':
    main()
