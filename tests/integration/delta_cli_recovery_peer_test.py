"""Deterministic transport interruption and persistent delta-client recovery."""
import hashlib
import json
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading

H = struct.Struct('!IHHIQQI')
cli = sys.argv[1]
sid = '11' * 16

def exact(sock, length):
    data = b''
    while len(data) < length:
        part = sock.recv(length - len(data))
        if not part:
            raise EOFError()
        data += part
    return data

def receive(sock):
    _, _, kind, _, rid, length, _ = H.unpack(exact(sock, 32))
    body = exact(sock, length)
    return kind, rid, json.loads(body) if kind != 5 else body

def send(sock, kind, rid, body):
    data = json.dumps(body, separators=(',', ':')).encode()
    sock.sendall(H.pack(0x43484B48, 1, kind, 0, rid, len(data), 0) + data)

def hello(sock):
    kind, rid, _ = receive(sock)
    assert kind == 1
    send(sock, 2, rid, dict(version=1, server_name='recovery-peer', capabilities=['handshake', 'upload', 'fixed_block_delta_v1'], max_control_payload=65536, max_data_payload=1048576))

with tempfile.TemporaryDirectory(prefix='chunkharbor-client-recovery-') as temporary:
    root = Path(temporary)
    source, state = root / 'source', root / 'state'
    source.write_bytes(b'ab')
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen(1)
    port = listener.getsockname()[1]
    errors = []
    def run_peer(handler, target=listener):
        def serve():
            try:
                sock, _ = target.accept()
                with sock:
                    sock.settimeout(15)
                    hello(sock)
                    handler(sock)
            except Exception as error:
                errors.append(repr(error))
        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        return thread
    def command(selected_port=port):
        return [cli, 'backup', '--file', str(source), '--path', 'file', '--state', str(state), '--port', str(selected_port), '--chunk-size', '1']
    def initial(sock):
        kind, rid, begin = receive(sock)
        assert kind == 24
        send(sock, 25, rid, dict(session_id=sid, base_version=None, block_count=2))
        kind, rid, _ = receive(sock)
        assert kind == 18
        send(sock, 26, rid, dict(session_id=sid, next_page_index=1, phase='COLLECTING'))
        kind, rid, _ = receive(sock)
        assert kind == 19
        send(sock, 26, rid, dict(session_id=sid, next_page_index=1, phase='PLAN_READY'))
        kind, rid, _ = receive(sock)
        assert kind == 27
        send(sock, 20, rid, dict(session_id=sid, base_version=None, page_index=0, missing_ranges=[[0, 2]]))
        kind, rid, _ = receive(sock)
        assert kind == 27
        send(sock, 21, rid, dict(session_id=sid, base_version=None, page_count=1, block_count=2, missing_blocks=2, missing_bytes=2, reused_blocks=0, reused_bytes=0, change_kind='Added'))
        kind, rid, chunk = receive(sock)
        assert kind == 5
        send(sock, 6, rid, dict(session_id=sid, offset=0, data_length=1, chunk_sha256=chunk[28:60].hex(), durable=True))
        # Close before confirming block 1. Server recovery below reports only block 1 missing.
        assert receive(sock)[0] == 5
    thread = run_peer(initial)
    first = subprocess.run(command(), capture_output=True, text=True, timeout=20)
    thread.join(15)
    assert first.returncode != 0 and state.exists(), (first.stdout, first.stderr)
    def refuse_request(sock):
        try:
            receive(sock)
            errors.append('mismatched state continued to network request')
        except EOFError:
            pass
    # Source identity mismatch is detected after handshake, before QUERY_DELTA.
    source.write_bytes(b'ac')
    thread = run_peer(refuse_request)
    changed = subprocess.run(command(), capture_output=True, text=True, timeout=20)
    thread.join(15)
    assert changed.returncode != 0 and 'mismatch' in changed.stderr
    source.write_bytes(b'ab')
    # Linux state includes mtime; restore it from the saved decimal identity for recovery.
    import os, shlex
    fields = shlex.split(state.read_text().splitlines()[1])
    mtime = int(fields[5])
    if mtime:
        os.utime(source, ns=(mtime, mtime))
    other = socket.socket()
    other.bind(('127.0.0.1', 0))
    other.listen(1)
    thread = run_peer(refuse_request, other)
    wrong = subprocess.run(command(other.getsockname()[1]), capture_output=True, text=True, timeout=20)
    thread.join(15)
    other.close()
    assert wrong.returncode != 0 and 'mismatch' in wrong.stderr
    def recover(sock):
        kind, rid, body = receive(sock)
        assert kind == 28 and body['session_id'] == sid
        send(sock, 29, rid, dict(session_id=sid, mode='upload', phase='TRANSFERRING', base_version=None, next_manifest_page=1, missing_bitmap='02', result=None))
        kind, rid, chunk = receive(sock)
        assert kind == 5 and int.from_bytes(chunk[16:24], 'big') == 1
        send(sock, 6, rid, dict(session_id=sid, offset=1, data_length=1, chunk_sha256=chunk[28:60].hex(), durable=True))
        kind, rid, _ = receive(sock)
        assert kind == 30
        send(sock, 31, rid, dict(session_id=sid, version_id='33' * 16, path='file', size=2, sha256=hashlib.sha256(b'ab').hexdigest(), committed=True))
    thread = run_peer(recover)
    resumed = subprocess.run(command(), capture_output=True, text=True, timeout=20)
    thread.join(15)
    listener.close()
    assert resumed.returncode == 0 and 'blocks_sent_this_run=1' in resumed.stdout and not state.exists(), (resumed.stdout, resumed.stderr)
    assert not errors, errors
    print('delta client source/endpoint rejection and missing-bitmap recovery passed')
