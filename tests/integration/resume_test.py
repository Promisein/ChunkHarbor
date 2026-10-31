"""M3 restart/query verification. Only touches this test's temporary backup root."""
import hashlib
import json
import os
import selectors
import sqlite3
import socket
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SERVER = sys.argv[1]
HEADER = struct.Struct('!IHHIQQI')

def frame(kind, request, payload):
    if not isinstance(payload, bytes):
        payload = json.dumps(payload).encode()
    return HEADER.pack(0x43484b48, 1, kind, 0, request, len(payload), 0) + payload

def exact(sock, count):
    result = bytearray()
    while len(result) < count:
        part = sock.recv(count - len(result))
        assert part, 'unexpected server disconnect'
        result.extend(part)
    return bytes(result)

def exchange(sock, kind, request, payload, expected):
    sock.sendall(frame(kind, request, payload))
    magic, version, response_type, flags, response_id, length, reserved = HEADER.unpack(exact(sock, 32))
    assert (magic, version, flags, response_id, reserved) == (0x43484b48, 1, 0, request, 0)
    assert length <= 65536
    body = json.loads(exact(sock, length))
    assert response_type == expected, (response_type, body)
    return body

def start(root):
    process = subprocess.Popen([SERVER, '--upload', '--root', str(root), '--port', '0'],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    with selectors.DefaultSelector() as poll:
        poll.register(process.stdout, selectors.EVENT_READ)
        assert poll.select(10), 'server startup timeout: ' + process.stderr.read()
    line = process.stdout.readline().strip()
    assert line.startswith('LISTENING 127.0.0.1:'), (line, process.stderr.read())
    return process, int(line.rsplit(':', 1)[1])

def connect(port):
    sock = socket.create_connection(('127.0.0.1', port), timeout=10)
    exchange(sock, 1, 1, {'client_name':'resume-test','versions':[1],
                           'capabilities':['handshake','upload']}, 2)
    return sock

def missing_ranges(status):
    bitmap = bytes.fromhex(status['missing_bitmap'])
    assert len(bitmap) == (status['block_count'] + 7) // 8
    missing = [i for i in range(status['block_count']) if bitmap[i // 8] & (1 << (i % 8))]
    ranges = []
    for index in missing:
        if ranges and ranges[-1][1] == index: ranges[-1][1] += 1
        else: ranges.append([index, index + 1])
    return ranges

def chunk(sid, data, offset):
    return bytes.fromhex(sid) + struct.pack('!QI', offset, len(data)) + hashlib.sha256(data).digest() + data

def stop(process, crash=False):
    if crash: process.kill()
    else: process.terminate()
    process.wait(timeout=10)
    process.stdout.close()
    process.stderr.close()

with tempfile.TemporaryDirectory(prefix='chunkharbor-resume-') as temp:
    root = Path(temp) / 'backup'
    root.mkdir(mode=0o700)
    content = b'abcdefghij'
    meta = {'path':'restore/restart.bin', 'size':len(content), 'mtime_ns':0,
            'chunk_size':4, 'sha256':hashlib.sha256(content).hexdigest()}
    process, port = start(root)
    try:
        with connect(port) as sock:
            plan = exchange(sock, 3, 2, meta, 4)
            sid = plan['session_id']
            assert plan['missing_ranges'] == [[0, 3]]
            first = chunk(sid, content[:4], 0)
            assert exchange(sock, 5, 3, first, 6)['durable'] is True
            assert exchange(sock, 5, 4, first, 6)['durable'] is True
    finally:
        stop(process, crash=True)
    process, port = start(root)
    try:
        with connect(port) as sock:
            query = {'session_id':sid}
            assert missing_ranges(exchange(sock, 7, 2, query, 8)) == [[1, 3]]
            exchange(sock, 5, 3, chunk(sid, content[4:8], 4), 6)
            exchange(sock, 5, 4, chunk(sid, content[8:], 8), 6)
            result = exchange(sock, 9, 5, query, 10)
            assert result['sha256'] == meta['sha256']
        with connect(port) as sock:
            assert exchange(sock, 7, 2, query, 10)['committed'] is True
        with connect(port) as sock:
            assert exchange(sock, 9, 2, query, 10)['sha256'] == meta['sha256']
        with connect(port) as sock:
            assert exchange(sock, 7, 2, {'session_id':'0'*32}, 15)['code'] == 'SESSION_NOT_FOUND'
    finally:
        stop(process)
    assert (root / 'files' / meta['path']).read_bytes() == content
    # A durable DB row does not authorize corrupt bytes after restart.
    second = {**meta, 'path':'corrupt.bin'}
    process, port = start(root)
    try:
        with connect(port) as sock:
            assert exchange(sock, 7, 2, {'session_id':sid}, 10)['sha256'] == meta['sha256']
        with connect(port) as sock:
            assert exchange(sock, 9, 2, {'session_id':sid}, 10)['committed'] is True
        with connect(port) as sock:
            sid2 = exchange(sock, 3, 2, second, 4)['session_id']
            exchange(sock, 5, 3, chunk(sid2, content[:4], 0), 6)
    finally:
        stop(process, crash=True)
    with open(root / '.sessions' / sid2 / 'data', 'r+b') as data:
        data.write(b'zzzz')
        data.flush()
        os.fsync(data.fileno())
    with sqlite3.connect(root / 'metadata.sqlite3') as db:
        db.execute("UPDATE transfers SET state='COMMITTING' WHERE session_id=?", (sid2,))
    process, port = start(root)
    try:
        with connect(port) as sock:
            query = {'session_id':sid2}
            assert missing_ranges(exchange(sock, 7, 2, query, 8)) == [[0, 3]]
            for index in range(3):
                offset = index * 4
                exchange(sock, 5, 3 + index, chunk(sid2, content[offset:offset+4], offset), 6)
            assert exchange(sock, 9, 6, query, 10)['sha256'] == meta['sha256']
    finally:
        stop(process)
    assert (root / 'files' / 'corrupt.bin').read_bytes() == content
    # A synchronized data file without a committed SQLite row must not be ACKed.
    third = {**meta, 'path':'db-fault.bin'}
    process, port = start(root)
    try:
        with connect(port) as sock:
            sid3 = exchange(sock, 3, 2, third, 4)['session_id']
        with sqlite3.connect(root / 'metadata.sqlite3') as db:
            db.execute("CREATE TRIGGER reject_chunk BEFORE INSERT ON transfer_chunks "
                       "BEGIN SELECT RAISE(ABORT, 'injected failure'); END")
        with connect(port) as sock:
            assert missing_ranges(exchange(sock, 7, 2, {'session_id':sid3}, 8)) == [[0, 3]]
            assert exchange(sock, 5, 3, chunk(sid3, content[:4], 0), 15)['code'] == 'INTERNAL_ERROR'
        with sqlite3.connect(root / 'metadata.sqlite3') as db:
            db.execute('DROP TRIGGER reject_chunk')
    finally:
        stop(process, crash=True)
    process, port = start(root)
    try:
        with connect(port) as sock:
            assert missing_ranges(exchange(sock, 7, 2, {'session_id':sid3}, 8)) == [[0, 3]]
    finally:
        stop(process)
    with sqlite3.connect(root / 'metadata.sqlite3') as db:
        db.execute('UPDATE transfers SET created_at_ns=0,updated_at_ns=1 WHERE session_id=?', (sid3,))
    process, port = start(root)
    try:
        assert not (root / '.sessions' / sid3).exists()
        with connect(port) as sock:
            assert exchange(sock, 7, 2, {'session_id':sid3}, 15)['code'] == 'SESSION_NOT_FOUND'
    finally:
        stop(process)
    missing_id = 'e' * 32
    with sqlite3.connect(root / 'metadata.sqlite3') as db:
        db.execute("INSERT INTO transfers(session_id,logical_path,expected_sha256,total_size,chunk_size,state,created_at_ns,updated_at_ns,mtime_ns,temp_path) "
                   "VALUES (?,?,?,?,?,'CREATED',0,0,0,?)",
                   (missing_id, 'missing-stage.bin', hashlib.sha256(b'').hexdigest(), 0, 4,
                    '.sessions/' + missing_id + '/data'))
    orphan = 'f' * 32
    (root / '.sessions' / orphan).mkdir(mode=0o700)
    process, port = start(root)
    try:
        assert (root / '.quarantine' / orphan).is_dir()
        assert not (root / '.sessions' / orphan).exists()
        with sqlite3.connect(root / 'metadata.sqlite3') as db:
            assert db.execute('SELECT state FROM transfers WHERE session_id=?', (missing_id,)).fetchone() == ('FAILED',)
        with connect(port) as sock:
            assert exchange(sock, 7, 2, {'session_id':missing_id}, 15)['code'] == 'SESSION_NOT_FOUND'
    finally:
        stop(process)
    (root / 'files' / meta['path']).unlink()
    failed = subprocess.run([SERVER, '--upload', '--root', str(root), '--port', '0'],
                            capture_output=True, text=True, timeout=10)
    assert failed.returncode != 0 and 'LISTENING' not in failed.stdout
print('resume integration passed')
