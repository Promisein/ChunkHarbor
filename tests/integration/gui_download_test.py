"""Real Qt page/controller/socket and Windows publication; private roots only."""
import hashlib
import json
import ctypes
import os
from pathlib import Path
import queue
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

PROBE = sys.argv[1]
HEADER = struct.Struct('!IHHIQQI')
VERSION = '12' * 16
LOGICAL = '中文/备份.bin'


def exact(peer, n):
    data = b''
    while len(data) < n:
        part = peer.recv(n - len(data))
        assert part, 'unexpected client disconnect'
        data += part
    return data


def request(peer):
    magic, version, kind, flags, rid, length, reserved = HEADER.unpack(exact(peer, 32))
    assert (magic, version, flags, reserved) == (0x43484B48, 1, 0, 0)
    assert length <= 65536
    return kind, rid, json.loads(exact(peer, length))


def send(peer, kind, rid, body):
    if not isinstance(body, bytes):
        body = json.dumps(body).encode()
    wire = HEADER.pack(0x43484B48, 1, kind, 0, rid, len(body), 0) + body
    # Split the header and body to exercise the incremental TCP parser.
    peer.sendall(wire[:11])
    peer.sendall(wire[11:])


def check(case, historical=False):
    capability = 'historical_download_v1' if historical else 'download_v1'
    data = b'' if case == 'empty' else (bytes(range(256)) * 2301 if case in ('multi', 'partial', 'cancel_transfer') else b'hello\x00\xff' * 100)
    sha = hashlib.sha256(data).hexdigest()
    errors = queue.Queue()
    with tempfile.TemporaryDirectory(prefix='chunkharbor-gui-download-') as temp:
        base = Path(temp)
        directory = base / '中文目录'
        directory.mkdir()
        output = directory / '恢复文件.bin'
        if case == 'exists': output.write_bytes(b'keep-original')
        if case == 'disk_error': output = base / 'missing' / 'out.bin'
        if case == 'reparse':
            directory.rmdir()
            outside = base / 'outside'
            outside.mkdir()
            # Use a test-owned junction, no administrator or symlink privilege needed.
            r = subprocess.run(['cmd', '/c', 'mklink', '/J', str(directory), str(outside)], capture_output=True)
            assert r.returncode == 0, r.stderr
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen()
            listener.settimeout(4)

            def serve():
                try:
                    if historical:
                        with listener.accept()[0] as history:
                            history.settimeout(10)
                            kind, rid, hello = request(history)
                            assert kind == 1 and 'version_history_v1' in hello['capabilities']
                            send(history, 2, rid, dict(version=1, server_name='history-peer',
                                capabilities=['handshake', 'version_history_v1'], max_control_payload=65536,
                                max_data_payload=1048576))
                            kind, rid, query = request(history)
                            assert kind == 34 and query['path'] == LOGICAL
                            send(history, 35, rid, dict(path=LOGICAL, next_before=None, versions=[
                                dict(version_id='56' * 16, size=1, sha256='cd' * 32,
                                     sequence=2, committed_at_s=None, current=True),
                                dict(version_id=VERSION, size=len(data),
                                     sha256='ab' * 32 if case == 'whole_hash' else sha,
                                     sequence=1, committed_at_s=None, current=False)]))
                            assert history.recv(1) == b''
                    try:
                        conn, _ = listener.accept()
                    except socket.timeout:
                        assert case in ('exists', 'disk_error', 'reparse')
                        return
                    with conn:
                        conn.settimeout(13)
                        kind, rid, hello = request(conn)
                        assert kind == 1 and capability in hello['capabilities']
                        send(conn, 2, rid, dict(version=1, server_name='download-peer',
                            capabilities=['handshake'] if case == 'unsupported' else ['handshake', capability], max_control_payload=65536,
                            max_data_payload=1048576))
                        if case == 'unsupported':
                            assert conn.recv(1) == b''
                            return
                        kind, rid, begin = request(conn)
                        assert kind == (36 if historical else 13) and begin == dict(path=LOGICAL, version_id=VERSION)
                        if case in ('cancel', 'close', 'destroy', 'timeout'):
                            assert conn.recv(1) == b''
                            return
                        if case == 'unavailable':
                            send(conn, 15, rid, dict(code='VERSION_UNAVAILABLE', message='Missing object'))
                            assert conn.recv(1) == b''
                            return
                        if case == 'conflict':
                            send(conn, 15, rid, dict(code='VERSION_CONFLICT', message='Current file version changed'))
                            assert conn.recv(1) == b''
                            return
                        if case == 'target_race': output.write_bytes(b'keep-raced')
                        if case == 'target_link_race':
                            sentinel = directory / 'sentinel'
                            sentinel.write_bytes(b'keep-sentinel')
                            assert ctypes.windll.kernel32.CreateHardLinkW(str(output), str(sentinel), None)
                        if case == 'parent_race':
                            try: directory.rename(base / 'moved')
                            except OSError: pass  # Windows ancestor handle prevents replacement.
                            else: raise AssertionError('parent rename was not blocked')
                        ready = dict(path=LOGICAL, version_id=VERSION, size=len(data), sha256=sha)
                        if case == 'whole_hash': ready['sha256'] = 'ab' * 32
                        if case == 'version': ready['version_id'] = '34' * 16
                        if case == 'path': ready['path'] = 'other'
                        if case == 'oversize': ready['size'] = 67108865
                        if case == 'float': ready['size'] = 1.0
                        send(conn, 32, rid + (case == 'request'), ready)
                        if case in ('version', 'path', 'oversize', 'float', 'request'):
                            assert conn.recv(1) == b''
                            return
                        offset = 0
                        while offset < len(data):
                            kind, rid, get = request(conn)
                            assert kind == 33 and get['version_id'] == VERSION and get['offset'] == offset
                            length = get['length']
                            assert 0 < length <= 262144
                            chunk = data[offset:offset + length]
                            digest = hashlib.sha256(chunk).digest()
                            if case == 'chunk_hash': chunk = bytes([chunk[0] ^ 1]) + chunk[1:]
                            if case == 'disconnect': return
                            body = bytes.fromhex('34' * 16 if case == 'chunk_version' else VERSION)
                            body += struct.pack('!QI', offset + (case == 'offset'), len(chunk)) + digest + chunk
                            if case == 'truncate':
                                conn.sendall(HEADER.pack(0x43484B48, 1, 14, 0, rid, len(body), 0) + body[:70])
                                return
                            if case == 'partial':
                                send(conn, 14, rid, body)
                                return
                            send(conn, 5 if case == 'type' else 14, rid, body)
                            if case == 'cancel_transfer':
                                # Cancellation may arrive before or after the next pull.
                                # Drain at most one bounded request, then wait for closure.
                                while conn.recv(4096): pass
                                return
                            if case in ('chunk_hash', 'chunk_version', 'offset', 'type'):
                                assert conn.recv(1) == b''
                                return
                            offset += length
                        assert conn.recv(1) == b''
                except BaseException as exc: errors.put(exc)

            worker = threading.Thread(target=serve, daemon=True)
            worker.start()
            mode = case if case in ('cancel', 'close', 'destroy', 'cancel_transfer', 'selection_switch') else 'once'
            if historical: mode = 'history_' + mode
            # Test timeout uses the production ten-second request deadline.
            r = subprocess.run([PROBE, str(listener.getsockname()[1]), LOGICAL, VERSION,
                str(len(data)), 'ab' * 32 if case == 'whole_hash' else sha, str(output), mode], capture_output=True, timeout=23,
                env=dict(os.environ, QT_QPA_PLATFORM='offscreen'))
            worker.join(14)
            assert not worker.is_alive(), case
            if not errors.empty(): raise AssertionError((case, repr(errors.get()), r.stdout, r.stderr))
        lines = [line[7:] for line in r.stdout.decode('utf-8').splitlines() if line.startswith('RESULT=')]
        assert r.returncode == 0 and lines, (case, r.returncode, r.stdout, r.stderr)
        result = json.loads(lines[-1])
        success = case in ('empty', 'normal', 'multi', 'parent_race', 'selection_switch')
        assert result['success'] == success, (case, result)
        if success:
            assert output.read_bytes() == data and hashlib.sha256(output.read_bytes()).hexdigest() == sha
            assert int(result['received']) == len(data)
        elif case == 'target_race': assert output.read_bytes() == b'keep-raced'
        elif case == 'target_link_race':
            assert output.read_bytes() == (directory / 'sentinel').read_bytes() == b'keep-sentinel'
        elif case == 'exists': assert output.read_bytes() == b'keep-original'
        else: assert not output.exists(), (case, result)
        assert not list(base.rglob('.chunkharbor-download-*')), (case, 'leaked temporary')
        if case == 'unavailable': assert '不可用' in result['status']
        if case == 'conflict': assert '刷新' in result['status']
        if case == 'cancel_transfer': assert 0 < int(result['received']) < len(data), result
        if case not in ('exists', 'disk_error', 'destroy', 'reparse'):
            assert result['duplicate_rejected'] and result['heartbeat'] > 0, (case, result)
        print('PASS', case, flush=True)


if __name__ == '__main__':
    if len(sys.argv) > 2 and sys.argv[2] == 'history':
        for case in ['empty', 'multi', 'selection_switch', 'whole_hash', 'chunk_hash', 'disconnect',
                     'cancel_transfer', 'exists', 'target_race', 'version', 'close', 'destroy',
                     'unavailable', 'unsupported']:
            check(case, historical=True)
        sys.exit(0)
    for case in ['empty', 'normal', 'multi', 'selection_switch', 'whole_hash', 'chunk_hash', 'disconnect',
                 'cancel', 'cancel_transfer', 'exists', 'target_race', 'target_link_race', 'reparse', 'version', 'conflict', 'close', 'destroy',
                 'timeout', 'disk_error', 'parent_race', 'path', 'oversize', 'float', 'request',
                 'chunk_version', 'offset', 'truncate', 'partial', 'type']:
        check(case)
