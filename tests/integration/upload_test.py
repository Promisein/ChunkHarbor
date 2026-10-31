"""M2 upload acceptance. Uses only isolated private temporary roots and sources."""
import concurrent.futures
import contextlib
import hashlib
import json
import os
import selectors
import socket
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SERVER, CLI = sys.argv[1:3]
HEADER = struct.Struct('!IHHIQQI')

def frame(value, kind=1, request=1):
    data = value if isinstance(value, bytes) else json.dumps(value).encode()
    return HEADER.pack(0x43484b48, 1, kind, 0, request, len(data), 0) + data

def exact(sock, count):
    data = bytearray()
    while len(data) < count:
        part = sock.recv(count-len(data))
        assert part, 'unexpected EOF'
        data.extend(part)
    return bytes(data)

def response(sock, kind, request):
    h = HEADER.unpack(exact(sock, 32))
    assert h[:5] == (0x43484b48, 1, kind, 0, request), h
    assert h[5] <= 65536 and h[6] == 0
    return json.loads(exact(sock, h[5]))

@contextlib.contextmanager
def server(root):
    p = subprocess.Popen([SERVER, '--upload', '--root', str(root), '--port', '0'],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        with selectors.DefaultSelector() as poll:
            poll.register(p.stdout, selectors.EVENT_READ)
            assert poll.select(10), 'server startup timeout'
            line = p.stdout.readline().strip()
        assert line.startswith('LISTENING 127.0.0.1:'), line
        yield int(line.rsplit(':', 1)[1])
    finally:
        p.terminate()
        try: p.wait(10)
        except subprocess.TimeoutExpired:
            p.kill(); p.wait(); raise AssertionError('server shutdown timeout')
        errors = p.stderr.read()
        assert p.returncode == 0, errors
        assert 'AddressSanitizer' not in errors and 'runtime error:' not in errors, errors

with tempfile.TemporaryDirectory(prefix='chunkharbor-upload-') as tmp:
    base = Path(tmp); root = base/'private'; root.mkdir(mode=0o700)
    with server(root) as port:
        def connect(): return socket.create_connection(('127.0.0.1', port), timeout=8)
        offer = {'client_name':'upload-test', 'versions':[1], 'capabilities':['handshake','upload']}
        def hello(sock):
            sock.sendall(frame(offer)); ack = response(sock, 2, 1)
            assert 'upload' in ack['capabilities']
        def begin(path='partial.bin', data=b'abcdef', chunk=4):
            return {'path':path, 'size':len(data), 'mtime_ns':0, 'chunk_size':chunk,
                    'sha256':hashlib.sha256(data).hexdigest()}
        def rejected(value, kind, code, handshaken=True):
            with connect() as sock:
                if handshaken: hello(sock)
                sock.sendall(frame(value, kind, 2))
                assert response(sock, 15, 2)['code'] == code
        rejected({}, 3, 'HANDSHAKE_REQUIRED', False)
        rejected(offer, 1, 'ALREADY_HANDSHAKEN')
        rejected(b'{', 3, 'MALFORMED_MESSAGE')
        rejected(begin('../escape'), 3, 'MALFORMED_MESSAGE')
        rejected(begin('/absolute'), 3, 'MALFORMED_MESSAGE')
        rejected({**begin(), 'size':64*1024*1024+1}, 3, 'QUOTA_EXCEEDED')
        rejected({'session_id':'00'*16}, 9, 'SESSION_NOT_FOUND')
        with connect() as sock:
            hello(sock); sock.sendall(frame(begin(),3,2)); plan = response(sock,4,2)
            sock.sendall(frame({'session_id':plan['session_id']},9,3))
            assert response(sock,15,3)['code'] == 'INCOMPLETE_UPLOAD'
        assert not (root/'files'/'partial.bin').exists()
        def chunk(session, data, offset=0, digest=None):
            return (bytes.fromhex(session) + struct.pack('!QI', offset, len(data)) +
                    (digest if digest is not None else hashlib.sha256(data).digest()) + data)
        # Isolated sessions exercise geometry, identity, digest, and commit checks.
        for i, fault in enumerate(('digest','offset','range','session','malformed','whole')):
            content = b'abcd'
            manifest = begin(f'bad-{fault}', content, 4)
            if fault == 'whole': manifest['sha256'] = '00'*32
            with connect() as sock:
                hello(sock); sock.sendall(frame(manifest,3,2)); plan = response(sock,4,2)
                sid = plan['session_id']
                payload = chunk(sid, content)
                expected = 'MALFORMED_MESSAGE'
                if fault == 'digest':
                    payload = chunk(sid,content,digest=b'\0'*32); expected='CHECKSUM_MISMATCH'
                elif fault == 'offset': payload = chunk(sid,content,offset=1)
                elif fault == 'range': payload = chunk(sid,content,offset=4)
                elif fault == 'session':
                    payload = chunk('00'*16,content); expected='SESSION_NOT_FOUND'
                elif fault == 'malformed': payload = b'truncated'
                sock.sendall(frame(payload,5,3))
                if fault == 'whole':
                    response(sock,6,3)
                    sock.sendall(frame({'session_id':sid},9,4))
                    assert response(sock,15,4)['code'] == 'CHECKSUM_MISMATCH'
                else: assert response(sock,15,3)['code'] == expected
            assert not (root/'files'/f'bad-{fault}').exists()
        with connect() as sock:
            hello(sock); sock.sendall(frame(begin('pipeline',b'abcd',4),3,2))
            sid = response(sock,4,2)['session_id']
            # Complete frames already buffered when the worker pauses must resume once.
            commit = {'session_id':sid}
            sock.sendall(frame(chunk(sid,b'abcd'),5,3) + frame(chunk(sid,b'abcd'),5,4) +
                         frame(commit,9,5) + frame(commit,9,6))
            assert response(sock,6,3)['durable'] is True
            response(sock,6,4); first = response(sock,10,5)
            assert response(sock,10,6) == first
        assert (root/'files'/'pipeline').read_bytes() == b'abcd'
        with connect() as sock:
            hello(sock); sock.sendall(frame(begin('abandoned',b'abcdefgh',4),3,2))
            sid = response(sock,4,2)['session_id']
            sock.sendall(frame(chunk(sid,b'abcd'),5,3)); response(sock,6,3)
        assert not (root/'files'/'abandoned').exists()
        rejected(begin('abandoned', b'other', 4),3,'CONFLICT')
        with connect() as sock:
            hello(sock); sock.sendall(frame(b'{',9,2))
            assert response(sock,15,2)['code'] == 'SESSION_NOT_FOUND'
        with connect() as sock:
            hello(sock)
            sock.sendall(frame(begin('duplicate-id',b'',4),3,1))
            assert response(sock,15,1)['code'] == 'MALFORMED_MESSAGE'
        outside = base/'outside'; outside.mkdir()
        (root/'files'/'symlink').symlink_to(outside,target_is_directory=True)
        with connect() as sock:
            hello(sock); sock.sendall(frame(begin('symlink/escape',b'',4),3,2))
            response(sock,15,2)
        assert not (outside/'escape').exists()
        def handshake_only(_):
            with connect() as sock: hello(sock)
        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
            list(pool.map(handshake_only,range(32)))
        def upload_case(index, data, logical):
            source = base/('source-'+str(index)); source.write_bytes(data)
            result = subprocess.run([CLI,'upload','--file',str(source),'--path',logical,
                       '--chunk-size','4096','--port',str(port)], capture_output=True,text=True,timeout=20)
            assert result.returncode == 0, (result.stdout,result.stderr)
            assert (root/'files'/logical).read_bytes() == data
        upload_case(0,b'', 'empty')
        upload_case(1,bytes(range(256))*40+b'last', '目录/二进制.bin')
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            list(pool.map(lambda i:upload_case(i+10,os.urandom(9000),f'parallel/{i}'),range(8)))
        # Existing destination must remain byte-identical after a rejected upload.
        source = base/'overwrite'; source.write_bytes(b'changed')
        result = subprocess.run([CLI,'upload','--file',str(source),'--path','empty',
                                '--port',str(port)],capture_output=True,text=True,timeout=10)
        assert result.returncode != 0 and (root/'files'/'empty').read_bytes() == b''
    # Restart accepts valid retained receipts and preserves published files;
    # it deliberately offers no QUERY_SESSION/restart-resume operation.
    with server(root) as restarted_port:
        source = base/'restart-source'; source.write_bytes(b'new upload after restart')
        result = subprocess.run([CLI,'upload','--file',str(source),'--path','after-restart',
                                 '--port',str(restarted_port)],capture_output=True,text=True,timeout=15)
        assert result.returncode == 0, (result.stdout,result.stderr)
        assert (root/'files'/'after-restart').read_bytes() == source.read_bytes()
        assert (root/'files'/'empty').read_bytes() == b''
print('upload: empty, binary, Unicode, concurrency, errors and no-overwrite passed')
