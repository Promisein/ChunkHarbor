"""Actual Qt file-page/controller against bounded loopback test peers."""
import json
import os
import queue
import socket
import struct
import subprocess
import sys
import threading
import time

HEADER = struct.Struct('!IHHIQQI')
PROBE = sys.argv[1]


def exact(peer, length):
    result = b''
    while len(result) < length:
        part = peer.recv(length - len(result))
        assert part, 'unexpected client disconnect'
        result += part
    return result


def request(peer):
    magic, version, kind, flags, rid, length, reserved = HEADER.unpack(exact(peer, 32))
    assert (magic, version, flags, reserved) == (0x43484B48, 1, 0, 0)
    assert length <= 65536
    return kind, rid, json.loads(exact(peer, length))


def frame(kind, rid, value):
    body = value if isinstance(value, bytes) else json.dumps(value).encode()
    return HEADER.pack(0x43484B48, 1, kind, 0, rid, len(body), 0) + body


def file(path, size=123):
    return dict(path=path, size=size, version_id='12' * 16, sha256='ab' * 32)


FIRST = dict(files=[file('a', 9007199254740993), file('中文/<b>.txt')], next_after_path='中文/<b>.txt')


def check(case):
    errors = queue.Queue()
    calls = []
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        listener.listen()
        listener.settimeout(15)
        port = listener.getsockname()[1]

        def serve():
            try:
                count = 3 if case == 'pages' else 2 if case in ('retain', 'nonadvance', 'restart') else 1
                for index in range(count):
                    with listener.accept()[0] as peer:
                        peer.settimeout(13)
                        kind, rid, hello = request(peer)
                        assert kind == 1 and rid == 1 and hello['capabilities'] == ['handshake', 'file_list_v1']
                        caps = ['handshake'] if case == 'no_capability' else ['handshake', 'file_list_v1']
                        peer.sendall(frame(2, rid, dict(version=1, server_name='files-peer', capabilities=caps,
                                                      max_control_payload=65536, max_data_payload=1048576)))
                        if case == 'no_capability':
                            assert peer.recv(1) == b''
                            continue
                        kind, rid, value = request(peer)
                        assert kind == 11 and rid == 2 and value['limit'] == 32
                        calls.append(value['after_path'])
                        if case == 'timeout':
                            assert peer.recv(1) == b''
                            continue
                        if case in ('cancel', 'endpoint', 'close') or (case == 'restart' and index == 0):
                            time.sleep(.2)
                            try: peer.sendall(frame(12, rid, FIRST))
                            except (BrokenPipeError, ConnectionResetError): pass
                            continue
                        reply = FIRST
                        typ = 12
                        if case == 'pages' and index == 1:
                            reply = dict(files=[file('龘')], next_after_path='')
                        elif case == 'restart': reply = dict(files=[file('new')], next_after_path='')
                        elif case == 'empty': reply = dict(files=[], next_after_path='')
                        elif case == 'duplicate': reply = dict(files=[file('a'), file('a')], next_after_path='')
                        elif case == 'cursor': reply = dict(files=[file('a')], next_after_path='different')
                        elif case == 'too_many': reply = dict(files=[file(f'a-{i:02}') for i in range(33)], next_after_path='')
                        elif case == 'error':
                            typ, reply = 15, dict(code='BUSY', message='queue full')
                        elif case == 'type': typ = 10
                        elif case == 'malformed': reply = b'{'
                        if (case == 'request') or (case == 'retain' and index == 1): rid += 1
                        if case == 'nonadvance' and index == 1: reply = dict(files=[file('a')], next_after_path='')
                        wire = frame(typ, rid, reply)
                        if case == 'oversize': wire = HEADER.pack(0x43484B48, 1, 12, 0, rid, 65537, 0)
                        if case == 'trailing': wire += b'x'
                        if case == 'disconnect': continue
                        if case == 'truncated': wire = wire[:40]
                        # At least one timer tick during a delayed asynchronous response.
                        time.sleep(.04)
                        if case == 'pages':
                            for start in range(0, len(wire), 13): peer.sendall(wire[start:start + 13])
                        else: peer.sendall(wire)
                        if case != 'truncated': assert peer.recv(1) == b'', 'extra request on completed page connection'
            except BaseException as exc: errors.put(exc)

        worker = threading.Thread(target=serve, daemon=True)
        worker.start()
        mode = case if case in ('pages', 'cancel', 'endpoint', 'close', 'restart') else 'retain' if case in ('retain', 'nonadvance') else 'once'
        r = subprocess.run([PROBE, str(port), mode], capture_output=True, timeout=19,
                           env=dict(os.environ, QT_QPA_PLATFORM='offscreen'))
        worker.join(15)
        assert not worker.is_alive(), case
        if not errors.empty(): raise AssertionError((case, repr(errors.get()), r.stderr))
        lines = [line[7:] for line in r.stdout.decode('utf-8').splitlines() if line.startswith('RESULT=')]
        assert r.returncode == 0 and lines, (case, r.returncode, r.stdout, r.stderr)
        result = json.loads(lines[-1])
        assert result['heartbeat'] >= 1, (case, result)
        if case == 'pages':
            assert calls == ['', '中文/<b>.txt', '']
            assert result['pages'] == [['a', '中文/<b>.txt'], ['龘'], ['a', '中文/<b>.txt']]
            assert result['display_size'] == '9007199254740993'
            assert result['visible'] == 1 and result['plain'] and '<b>' in result['detail']
            assert '<b>' not in result['tooltip'] and '&lt;b&gt;' in result['tooltip']
            assert result['next'] and not result['previous'] and not result['restore']
        elif case in ('retain', 'nonadvance'):
            assert not result['success'] and [f['path'] for f in result['rows']] == ['a', '中文/<b>.txt']
            assert '上次结果' in result['status'] and result['next'] and not result['previous']
        elif case in ('cancel', 'endpoint'):
            assert not result['busy'] and not result['rows']
            if case == 'endpoint': assert result['endpoint'] != f'127.0.0.1:{port}'
        elif case == 'close': assert not result['rows']
        elif case == 'restart': assert [f['path'] for f in result['rows']] == ['new'] and len(calls) == 2
        elif case == 'empty': assert result['success'] and not result['rows'] and '没有' in result['status']
        else:
            assert not result['success'] and not result['busy'] and not result['rows']
            if case == 'no_capability': assert '不支持' in result['status']
        print('PASS', case)


for scenario in ('pages', 'empty', 'retain', 'nonadvance', 'no_capability', 'duplicate', 'cursor',
                 'too_many', 'request', 'type', 'malformed', 'error', 'oversize', 'trailing',
                 'disconnect', 'truncated', 'cancel', 'endpoint', 'close', 'restart', 'timeout'):
    check(scenario)
