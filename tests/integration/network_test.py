"""M1 functional tests on loopback; this is not a benchmark."""
import concurrent.futures
import contextlib
import os
import selectors
import socket
import struct
import subprocess
import sys
import threading
import time

SERVER, CLI = sys.argv[1:3]
HEADER = struct.Struct('!IHHIQQI')

def frame(body=b'', kind=16, request=1):
    return HEADER.pack(0x43484b48, 1, kind, 0, request, len(body), 0) + body

def exact(sock, n):
    data = bytearray()
    while len(data) < n:
        part = sock.recv(n - len(data))
        if not part:
            raise AssertionError('premature EOF')
        data.extend(part)
    return bytes(data)

def reply(sock, body, kind=17, request=1):
    h = HEADER.unpack(exact(sock, 32))
    assert h == (0x43484b48, 1, kind, 0, request, len(body), 0), h
    assert exact(sock, len(body)) == body

def connect(port):
    return socket.create_connection(('127.0.0.1', port), timeout=8)

def closed(sock):
    try:
        assert sock.recv(1) == b''
    except ConnectionResetError:
        pass

@contextlib.contextmanager
def server(*options):
    process = subprocess.Popen([SERVER, '--echo', '--port', '0', *options],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            assert selector.select(10), 'server startup timed out'
            line = process.stdout.readline().strip()
        assert line.startswith('LISTENING 127.0.0.1:'), (line, process.stderr.read())
        yield process, int(line.rsplit(':', 1)[1])
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait(); raise AssertionError('shutdown stuck')
        errors = process.stderr.read()
        assert process.returncode == 0, (process.returncode, errors)
        assert 'AddressSanitizer' not in errors and 'runtime error:' not in errors, errors

with server() as (process, port):
    output = subprocess.check_output([CLI, 'echo', '--port', str(port), '--message', '你好 ChunkHarbor'], text=True)
    assert 'ECHO OK: 你好 ChunkHarbor' in output
    fd_dir = f'/proc/{process.pid}/fd'
    time.sleep(.05)
    baseline = len(os.listdir(fd_dir))
    clients = int(os.environ.get('CHUNKHARBOR_TEST_CLIENTS', '100'))
    barrier = threading.Barrier(clients)
    def client(i):
        with connect(port) as sock:
            barrier.wait(timeout=10)
            body = bytes([i % 256]) * 4097
            data = frame(body, request=i+1)
            sock.sendall(data[:1]); sock.sendall(data[1:13]); sock.sendall(data[13:])
            reply(sock, body, request=i+1)
            sock.shutdown(socket.SHUT_WR); closed(sock)
    with concurrent.futures.ThreadPoolExecutor(max_workers=clients) as executor:
        list(executor.map(client, range(clients)))
    with connect(port) as sock:
        bodies = [b'', b'a\0b', b'z' * 65536]
        sock.sendall(b''.join(frame(b, request=i+10) for i, b in enumerate(bodies)))
        for i, b in enumerate(bodies): reply(sock, b, request=i+10)
        sock.shutdown(socket.SHUT_WR); closed(sock)
    with connect(port) as sock:
        body = bytes(range(256)) * 4096
        sock.sendall(frame(body, kind=5)); sock.shutdown(socket.SHUT_WR)
        time.sleep(.1)  # response exceeds a single recv/send; peer reads later
        reply(sock, body, kind=5); closed(sock)
    for offset in [0, 5, 7, 8, 31]:
        bad = bytearray(frame()); bad[offset] = 255
        with connect(port) as sock:
            sock.sendall(bad); closed(sock)
    for kind, size in [(16,65537), (5,1048577), (5,2**64-1)]:
        with connect(port) as sock:
            sock.sendall(HEADER.pack(0x43484b48,1,kind,0,1,size,0)); closed(sock)
    with connect(port) as sock:  # known but unsupported upload operation
        sock.sendall(frame(b'', kind=3)); closed(sock)
    with connect(port) as sock:
        sock.sendall(frame(b'broken')[:35]); sock.shutdown(socket.SHUT_WR); closed(sock)
    for i in range(200):
        sock = connect(port)
        if i % 2:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii',1,0))
        sock.close()
    # A slow non-reader is bounded/backpressured and does not block a healthy client.
    with connect(port) as slow:
        slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        slow.settimeout(.15)
        payload = frame(b'x' * 1048576, kind=5)
        for _ in range(30):
            try: slow.sendall(payload)
            except socket.timeout: break
        with connect(port) as healthy:
            healthy.sendall(frame(b'alive')); reply(healthy, b'alive')
        # This is a generous process RSS safety check, not a performance metric.
        status = open(f'/proc/{process.pid}/status').read()
        rss = next(int(line.split()[1]) for line in status.splitlines() if line.startswith('VmRSS:'))
        # ASan reserves additional allocator/shadow memory; per-connection cap is also unit-tested.
        assert rss < 512 * 1024, rss
    deadline = time.monotonic() + 5
    while len(os.listdir(fd_dir)) != baseline and time.monotonic() < deadline: time.sleep(.05)
    assert len(os.listdir(fd_dir)) == baseline, (baseline, os.listdir(fd_dir))
    print(f'{clients} simultaneous clients, split/coalesced/1MiB/invalid frames, EOF/RST, slow reader and FD recovery passed')

with server('--idle-timeout-ms','300','--max-connections','1') as (process, port):
    with connect(port) as idle:
        time.sleep(.05)
        with connect(port) as excess: closed(excess)
        closed(idle)
    print('idle timeout, connection limit and controlled shutdown passed')

# Verify the Linux CLI against peers which accept but never complete a response.
for partial in (b'', b'CHKH'):
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0)); listener.listen(1); listener.settimeout(10)
        finish = threading.Event()
        def blackhole():
            with listener.accept()[0] as peer:
                peer.settimeout(8)
                header = HEADER.unpack(exact(peer, 32)); exact(peer, header[5])
                if partial: peer.sendall(partial)
                finish.wait(10)
        worker = threading.Thread(target=blackhole)
        worker.start()
        try:
            result = subprocess.run([CLI,'echo','--port',str(listener.getsockname()[1])],
                                    capture_output=True,text=True,timeout=12)
            assert result.returncode == 2 and 'timeout' in result.stderr, result
        finally:
            finish.set(); worker.join(12)
        assert not worker.is_alive()
print('CLI silent peer and truncated-header deadlines passed')
