"""Adversarial loopback download peers; destination and all races are test-owned."""
import hashlib
import json
from pathlib import Path
import os
import queue
import socket
import struct
import subprocess
import sys
import tempfile
import threading
from cli_upload_peer_test import receive

CLI = sys.argv[1]
HEADER = struct.Struct('!IHHIQQI')
VERSION = '12' * 16
DATA = b'a\x00bc' * 20000


def send(conn, kind, request, value):
    body = value if isinstance(value, bytes) else json.dumps(value).encode()
    wire = HEADER.pack(0x43484B48, 1, kind, 0, request, len(body), 0) + body
    # Fragment both header and body without relying on TCP write boundaries.
    for pos in range(0, len(wire), 997):
        conn.sendall(wire[pos:pos + 997])


def peer(listener, mode, output, result):
    try:
        with listener.accept()[0] as conn:
            conn.settimeout(8)
            kind, request, _ = receive(conn)
            assert kind == 1
            send(conn, 2, request, dict(version=1, server_name='download-peer',
                 capabilities=['handshake', 'download_v1'], max_control_payload=65536, max_data_payload=1048576))
            kind, request, body = receive(conn)
            assert kind == 13 and json.loads(body)['path'] == 'logical/file'
            digest = hashlib.sha256(DATA).hexdigest()
            ready = dict(path='logical/file', version_id=VERSION, size=len(DATA), sha256=digest)
            if mode == 'partial_transfer':
                ready['size'] = 262144 + len(DATA)
                ready['sha256'] = hashlib.sha256(b'A' * 262144 + DATA).hexdigest()
            if mode == 'path': ready['path'] = 'wrong'
            if mode == 'whole_hash': ready['sha256'] = '0' * 64
            if mode == 'too_large': ready['size'] = 67108865
            if mode == 'float_size': ready['size'] = float(len(DATA))
            if mode == 'version': ready['version_id'] = '34' * 16
            if mode == 'target_race': output.write_bytes(b'keep-raced-file')
            if mode == 'target_link_race': output.symlink_to(output.parent / 'sentinel')
            if mode == 'parent_race':
                # CLI already opened its directory and created its temporary file.
                output.parent.rename(output.parent.with_name('moved'))
                output.parent.symlink_to(output.parent.with_name('outside'), target_is_directory=True)
            send(conn, 32, request + (mode == 'request'), ready)
            if mode in ('path', 'too_large', 'float_size', 'version', 'request'):
                assert conn.recv(1) == b''
            else:
                kind, request, body = receive(conn)
                assert kind == 33
                get = json.loads(body)
                if mode == 'partial_transfer':
                    assert get['offset'] == 0 and get['length'] == 262144
                    part = b'A' * 262144
                    send(conn, 14, request, bytes.fromhex(VERSION) + struct.pack('!QI', 0, len(part)) +
                         hashlib.sha256(part).digest() + part)
                    kind, _, body = receive(conn)
                    assert kind == 33 and json.loads(body)['offset'] == 262144
                    result.put(None)
                    return
                assert get['version_id'] == VERSION and get['offset'] == 0 and get['length'] == len(DATA)
                data = DATA[:-1] if mode == 'length' else DATA
                version = '56' * 16 if mode == 'chunk_version' else VERSION
                offset = 1 if mode == 'offset' else 0
                digest = b'\0' * 32 if mode == 'chunk_hash' else hashlib.sha256(data).digest()
                body = bytes.fromhex(version) + struct.pack('!QI', offset, len(data)) + digest + data
                if mode == 'truncate':
                    conn.sendall(HEADER.pack(0x43484B48, 1, 14, 0, request, len(body), 0) + body[:100])
                else:
                    send(conn, 5 if mode == 'type' else 14, request, body)
        result.put(None)
    except BaseException as exc:
        result.put(exc)


def main():
    hook = sys.argv[2] if len(sys.argv) > 2 else None
    modes = ['enospc', 'filesync', 'dirsync', 'short_eintr'] if hook else [
        'ok', 'path', 'whole_hash', 'too_large', 'float_size', 'version', 'request',
        'chunk_version', 'offset', 'length', 'chunk_hash', 'truncate', 'type',
        'target_race', 'target_link_race', 'parent_race', 'partial_transfer']
    for mode in modes:
        with tempfile.TemporaryDirectory(prefix='chunkharbor-download-peer-') as temp:
            base = Path(temp)
            directory = base / 'output'
            directory.mkdir()
            (base / 'outside').mkdir()
            (directory / 'sentinel').write_bytes(b'keep-sentinel')
            output = directory / 'restored'
            with socket.socket() as listener:
                listener.bind(('127.0.0.1', 0))
                listener.listen()
                listener.settimeout(8)
                results = queue.Queue()
                worker = threading.Thread(target=peer, args=(listener, mode, output, results), daemon=True)
                worker.start()
                env = dict(os.environ)
                if hook:
                    env['LD_PRELOAD'] = hook
                    env['CHUNKHARBOR_DOWNLOAD_FAULT'] = mode
                r = subprocess.run([CLI, 'download', '--port', str(listener.getsockname()[1]),
                                    '--path', 'logical/file', '--output', str(output),
                                    '--version-id', VERSION], capture_output=True, timeout=12, env=env)
                worker.join(10)
                assert not worker.is_alive(), mode
                outcome = results.get_nowait()
                assert outcome is None, (mode, repr(outcome), r.stderr)
            success = mode in ('ok', 'parent_race', 'short_eintr')
            assert r.returncode == (0 if success else 2), (mode, r.stdout, r.stderr)
            assert (b'DOWNLOAD OK' in r.stdout) == success
            if mode == 'parent_race':
                assert (base / 'moved' / 'restored').read_bytes() == DATA
                assert not (base / 'outside' / 'restored').exists()
            elif mode in ('ok', 'short_eintr', 'dirsync'):
                assert output.read_bytes() == DATA
            elif mode == 'target_race':
                assert output.read_bytes() == b'keep-raced-file'
            elif mode == 'target_link_race':
                assert output.is_symlink() and output.read_bytes() == b'keep-sentinel'
            else:
                assert not output.exists(), (mode, 'published invalid download')
            assert not list(base.rglob('.chunkharbor-download-*')), mode
            if mode != 'parent_race':
                assert (directory / 'sentinel').read_bytes() == b'keep-sentinel'
            print('PASS', mode)


if __name__ == '__main__':
    main()
