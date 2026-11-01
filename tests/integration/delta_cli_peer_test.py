"""Malicious delta peers: the CLI must fail closed and retain recovery state."""
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading

HEADER = struct.Struct('!IHHIQQI')
cli = sys.argv[1]

def exact(sock, length):
    data = b''
    while len(data) < length:
        part = sock.recv(length - len(data))
        if not part:
            raise EOFError()
        data += part
    return data

def receive(sock):
    _, _, kind, _, rid, length, _ = HEADER.unpack(exact(sock, 32))
    body = exact(sock, length)
    return kind, rid, json.loads(body) if kind != 5 else body

def send(sock, kind, rid, body):
    encoded = json.dumps(body, separators=(',', ':')).encode()
    sock.sendall(HEADER.pack(0x43484B48, 1, kind, 0, rid, len(encoded), 0) + encoded)

with tempfile.TemporaryDirectory(prefix='chunkharbor-delta-peer-') as temporary:
    root = Path(temporary)
    source = root / 'source'
    source.write_bytes(b'a')
    for scenario in ['wrong-ready', 'wrong-ack', 'missing-done', 'false-result', 'state-failure']:
        listener = socket.socket()
        listener.bind(('127.0.0.1', 0))
        listener.listen(1)
        port = listener.getsockname()[1]
        errors = []
        def peer():
            try:
                sock, _ = listener.accept()
                with sock:
                    sock.settimeout(10)
                    _, rid, _ = receive(sock)
                    send(sock, 2, rid, dict(version=1, server_name='fake', capabilities=['handshake', 'upload', 'fixed_block_delta_v1'], max_control_payload=65536, max_data_payload=1048576))
                    _, rid, begin = receive(sock)
                    sid = '11' * 16
                    send(sock, 25, rid + (scenario == 'wrong-ready'), dict(session_id=sid, base_version=None, block_count=1))
                    if scenario in ['wrong-ready', 'state-failure']:
                        try:
                            receive(sock)
                            errors.append('client continued after invalid ready/state write failure')
                        except EOFError:
                            pass
                        return
                    _, rid, _ = receive(sock)
                    send(sock, 26, rid, dict(session_id='22' * 16 if scenario == 'wrong-ack' else sid, next_page_index=1, phase='COLLECTING'))
                    if scenario == 'wrong-ack':
                        return
                    _, rid, _ = receive(sock)
                    send(sock, 26, rid, dict(session_id=sid, next_page_index=1, phase='PLAN_READY'))
                    _, rid, _ = receive(sock)
                    send(sock, 20, rid, dict(session_id=sid, base_version=None, page_index=0, missing_ranges=[[0, 1]]))
                    _, rid, _ = receive(sock)
                    if scenario == 'missing-done':
                        return
                    send(sock, 21, rid, dict(session_id=sid, base_version=None, page_count=1, block_count=1, missing_blocks=1, missing_bytes=1, reused_blocks=0, reused_bytes=0, change_kind="Added"))
                    kind, rid, chunk = receive(sock)
                    assert kind == 5
                    # CHUNK_ACK uses session/offset/length/sha256; mirror exact request.
                    send(sock, 6, rid, dict(session_id=sid, offset=0, data_length=1, chunk_sha256=chunk[28:60].hex(), durable=True))
                    try:
                        _, rid, _ = receive(sock)
                    except EOFError:
                        errors.append("client rejected valid transfer before false result")
                        return
                    send(sock, 31, rid, dict(session_id=sid, version_id='33' * 16, path='wrong', size=1, sha256=begin['metadata']['sha256'], committed=True))
            except Exception as error:
                errors.append(str(error))
        thread = threading.Thread(target=peer, daemon=True)
        thread.start()
        state = root / scenario
        if scenario == 'state-failure':
            state = root / 'missing-parent' / 'state'
        result = subprocess.run([cli, 'backup', '--file', str(source), '--path', 'file', '--state', str(state), '--port', str(port), '--chunk-size', '1'], capture_output=True, text=True, timeout=20)
        thread.join(timeout=12)
        listener.close()
        assert result.returncode != 0, (scenario, result.stdout, result.stderr)
        assert not thread.is_alive(), scenario
        assert not errors, (scenario, errors)
    print('delta malicious peer rejection passed')
