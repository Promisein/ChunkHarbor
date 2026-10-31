import json, os, queue, socket, struct, subprocess, sys, threading, time
HEADER = struct.Struct('!IHHIQQI')
def exact(peer, count):
    data = b''
    while len(data) < count:
        part = peer.recv(count-len(data))
        assert part, 'GUI closed before HELLO'
        data += part
    return data
def check(exe, case):
    errors = queue.Queue()
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0)); listener.listen(1); listener.settimeout(8)
        def serve():
            try:
                with listener.accept()[0] as peer:
                    peer.settimeout(8)
                    magic, version, kind, flags, request, length, reserved = HEADER.unpack(exact(peer, 32))
                    assert (magic, version, kind, flags, request, reserved) == (0x43484b48, 1, 1, 0, 1, 0)
                    assert length <= 4096
                    assert json.loads(exact(peer, length)) == {'client_name':'chunkharbor-gui','versions':[1],'capabilities':['handshake']}
                    if case == 'disconnect': return
                    if case == 'timeout':
                        assert peer.recv(1) == b''
                        return
                    ack = dict(version=1, server_name='test-peer', capabilities=['handshake'], max_control_payload=65536, max_data_payload=1048576)
                    if case == 'boolean': ack['max_control_payload'] = True
                    body = json.dumps(ack).encode()
                    frame = HEADER.pack(0x43484b48, 1, 2, 0, 2 if case == 'request' else 1, len(body), 0) + body
                    if case == 'oversize': frame = HEADER.pack(0x43484b48, 1, 2, 0, 1, 4097, 0)
                    if case == 'trailing': frame += b'x'
                    if case == 'valid':
                        for piece in (frame[:3],frame[3:31],frame[31:45],frame[45:]):
                            peer.sendall(piece); time.sleep(.02)
                    else: peer.sendall(frame)
                    assert peer.recv(1) == b''
            except BaseException as error: errors.put(error)
        thread = threading.Thread(target=serve, daemon=True); thread.start()
        result = subprocess.run([exe,'--probe-port',str(listener.getsockname()[1])], env=dict(os.environ,QT_QPA_PLATFORM='offscreen'),capture_output=True,timeout=9)
        thread.join(9)
        assert not thread.is_alive(), case
        if not errors.empty(): raise errors.get()
        assert result.returncode == (0 if case == 'valid' else 2), (case,result.returncode,result.stdout,result.stderr)
for case in ('valid','request','boolean','disconnect','oversize','trailing','timeout'): check(sys.argv[1],case)
print('Qt handshake probe: 7 cases passed')
