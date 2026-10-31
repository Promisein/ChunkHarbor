"""Strict M2 handshake only: no file transfer or authentication."""
import concurrent.futures
import contextlib
import json
import selectors
import socket
import struct
import subprocess
import sys
import threading

SERVER, CLI = sys.argv[1:3]
HEADER = struct.Struct('!IHHIQQI')
OFFER = {'client_name': 'test-client', 'versions': [1], 'capabilities': ['handshake','future']}

def frame(value, kind=1, request=1):
    payload = value if isinstance(value, bytes) else json.dumps(value).encode()
    return HEADER.pack(0x43484b48,1,kind,0,request,len(payload),0) + payload

def exact(sock, n):
    data = bytearray()
    while len(data) < n:
        part = sock.recv(n-len(data))
        assert part, 'unexpected EOF'
        data.extend(part)
    return bytes(data)

def response(sock, kind, request):
    header = HEADER.unpack(exact(sock,32))
    assert header[:4] == (0x43484b48,1,kind,0) and header[4] == request and header[6] == 0
    assert header[5] <= 4096
    return json.loads(exact(sock,header[5]))

def closed(sock):
    try: assert sock.recv(1) == b''
    except ConnectionResetError: pass

@contextlib.contextmanager
def server():
    process = subprocess.Popen([SERVER,'--handshake','--port','0'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    try:
        with selectors.DefaultSelector() as ready:
            ready.register(process.stdout,selectors.EVENT_READ)
            assert ready.select(10), 'server not ready'
            line = process.stdout.readline().strip()
        assert line.startswith('LISTENING 127.0.0.1:'), line
        yield int(line.rsplit(':',1)[1])
    finally:
        process.terminate()
        try: process.wait(5)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait(); raise AssertionError('shutdown stuck')
        errors = process.stderr.read()
        assert process.returncode == 0, errors
        assert 'AddressSanitizer' not in errors and 'runtime error:' not in errors, errors

with server() as port:
    def connect(): return socket.create_connection(('127.0.0.1',port),timeout=5)
    result = subprocess.run([CLI,'hello','--port',str(port)],capture_output=True,text=True,timeout=8)
    assert result.returncode == 0 and 'HELLO OK: version=1' in result.stdout, result
    def handshake(i):
        with connect() as sock:
            data = frame(OFFER,request=i+1)
            for part in (data[:1],data[1:9],data[9:31],data[31:]): sock.sendall(part)
            sock.shutdown(socket.SHUT_WR)
            ack = response(sock,2,i+1)
            assert ack == {'version':1,'server_name':'chunkharbor-server','capabilities':['handshake'],
                           'max_control_payload':65536,'max_data_payload':1048576}, ack
            closed(sock)
    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as executor:
        list(executor.map(handshake,range(32)))
    invalid = [b'',b'{}',b'[]',b'{"client_name":1}',b'null', b'\xef\xbb\xbf{}',
               b'{"client_name":"a","versions":[1.0],"capabilities":[]}',
               b'{"client_name":"a","versions":[true],"capabilities":[]}',
               b'{"client_name":"a","versions":[1],"capabilities":[],"versions":[1]}',
               b'{"client_name":"a","versions":[1],"capabilities":[],"extra":1}',
               b'{"client_name":"a\\u0000","versions":[1],"capabilities":[]}',
               b'{"client_name":"\xff","versions":[1],"capabilities":[]}',
               b'['*1000 + b']'*1000, b' '*4097]
    valid_hello = json.dumps(OFFER).encode()
    invalid.extend(valid_hello + suffix for suffix in
                   (b'\0', b'\0garbage', b'\0' + valid_hello))
    for body in invalid:
        with connect() as sock:
            sock.sendall(frame(body,request=91))
            assert response(sock,15,91)['code'] == 'MALFORMED_MESSAGE'
            closed(sock)
    for offer, kind, request, error in [
        ({**OFFER,'versions':[2]},1,55,'UNSUPPORTED_VERSION'),
        (OFFER,1,0,'MALFORMED_MESSAGE'),
        ({},3,7,'HANDSHAKE_REQUIRED')]:
        with connect() as sock:
            sock.sendall(frame(offer,kind,request)); assert response(sock,15,request)['code'] == error
            closed(sock)
    for kind, error in [(1,'ALREADY_HANDSHAKEN'),(3,'UNSUPPORTED_OPERATION')]:
        with connect() as sock:
            sock.sendall(frame(OFFER,request=100) + frame(OFFER,kind,101))
            response(sock,2,100); assert response(sock,15,101)['code'] == error; closed(sock)
    # A fresh connection must not inherit READY from a released/reused FD.
    handshake(1000)
    with connect() as sock:
        sock.sendall(frame({},3,1002)); assert response(sock,15,1002)['code'] == 'HANDSHAKE_REQUIRED'
    with connect() as sock:
        sock.sendall(frame(OFFER)[:35]); sock.shutdown(socket.SHUT_WR); closed(sock)
    result = subprocess.run([CLI,'echo','--port',str(port)],capture_output=True,text=True,timeout=8)
    assert result.returncode == 2
    print('strict schemas, fragmented/coalesced frames, 32 connections, per-connection state and mode isolation passed')

# Client rejects forged/mismatched/ill-typed ACKs and surfaces structured errors.
ack = {'version':1,'server_name':'fake','capabilities':['handshake'],
       'max_control_payload':65536,'max_data_payload':1048576}
error_body = {'code':'UNSUPPORTED_VERSION','message':'No common protocol version'}
malformed_responses = []
for valid_body, kind in [(ack, 2), (error_body, 15)]:
    raw = json.dumps(valid_body).encode()
    malformed_responses.extend((raw + suffix, kind, 1) for suffix in
                               (b'\0', b'\0garbage', b'\0' + raw))
for value, kind, request in [(ack,2,99), ({**ack,'version':2},2,1),
        ({**ack,'capabilities':['upload']},2,1), ({**ack,'max_control_payload':65536.0},2,1),
        ({**ack,'max_data_payload':1048577},2,1),
        (error_body,15,1)] + malformed_responses:
    with socket.socket() as listener:
        listener.bind(('127.0.0.1',0)); listener.listen(1); listener.settimeout(8)
        def fake():
            with listener.accept()[0] as peer:
                peer.settimeout(5); header = HEADER.unpack(exact(peer,32)); exact(peer,header[5])
                peer.sendall(frame(value,kind,request))
        worker = threading.Thread(target=fake); worker.start()
        try:
            result = subprocess.run([CLI,'hello','--port',str(listener.getsockname()[1])],capture_output=True,text=True,timeout=8)
            assert result.returncode == 2 and 'HELLO OK' not in result.stdout, result
            if isinstance(value, bytes):
                assert 'MALFORMED_MESSAGE' in result.stderr, result
        finally: worker.join(8)
        assert not worker.is_alive()
print('client rejects invalid ACK correlation, version, capabilities and limits')
