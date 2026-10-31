#!/usr/bin/env python3
"""Adversarial loopback peers for CLI upload; all source data is temporary."""
import hashlib
import json
import queue
import socket
import struct
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

HEADER = struct.Struct("!IHHIQQI")


def receive(sock):
    def exact(count):
        result = b""
        while len(result) < count:
            part = sock.recv(count - len(result))
            if not part:
                raise EOFError("client closed")
            result += part
        return result

    magic, version, kind, flags, request, length, reserved = HEADER.unpack(exact(32))
    assert magic == 0x43484B48 and version == 1 and not flags and not reserved
    assert 0 < request and length <= 1048576
    return kind, request, exact(length)


def send(sock, kind, request, body):
    payload = json.dumps(body).encode()
    wire = HEADER.pack(0x43484B48, 1, kind, 0, request, len(payload), 0) + payload
    # Fragment header and body without assuming TCP preserves these writes.
    for start in range(0, len(wire), 7):
        sock.sendall(wire[start:start + 7])


def peer(listener, mode, source, results):
    try:
        with listener.accept()[0] as conn:
            conn.settimeout(5)
            kind, req, _ = receive(conn)
            assert kind == 1
            send(conn, 2, req, dict(
                version=1, server_name="fake", capabilities=["handshake", "upload"],
                max_control_payload=65536, max_data_payload=1048576))
            kind, req, body = receive(conn)
            assert kind == 3
            begin = json.loads(body)
            assert begin["size"] == 3 and begin["mtime_ns"] == source.stat().st_mtime_ns
            session = "01" * 16
            if mode == "source_changed":
                # BEGIN follows initial hash/stat. Change size before issuing PLAN,
                # so the next pre-chunk identity check must fail deterministically.
                source.write_bytes(b"changed source")
            send(conn, 4, req, dict(
                session_id=session, chunk_size=begin["chunk_size"], missing_ranges=[[0, 1]]))
            if mode == "source_changed":
                # Any request here, including COMMIT, is an error. EOF is required.
                assert conn.recv(1) == b"", "client sent a request after source mutation"
                results.put(None)
                return
            kind, req, body = receive(conn)
            assert kind == 5 and body[60:] == b"abc"
            send(conn, 6, req, dict(
                session_id=session, offset=0, data_length=3,
                chunk_sha256=hashlib.sha256(b"abc").hexdigest(), durable=mode != "ack_false"))
            if mode == "ack_false":
                assert conn.recv(1) == b"", "client continued after invalid ACK"
                results.put(None)
                return
            kind, req, _ = receive(conn)
            assert kind == 9
            result = dict(session_id=session, path=begin["path"], size=3,
                          sha256=begin["sha256"], committed=True)
            if mode == "truncate":
                conn.sendall(HEADER.pack(0x43484B48, 1, 10, 0, req, 100, 0) + b"{")
            else:
                if mode == "path": result["path"] = "wrong"
                if mode == "size": result["size"] = 4
                if mode == "hash": result["sha256"] = "a" * 64
                if mode == "session": result["session_id"] = "02" * 16
                send(conn, 2 if mode == "type" else 10,
                     req + 1 if mode == "request" else req, result)
        results.put(None)
    except BaseException as exc:
        results.put(exc)


def main():
    cli = sys.argv[1]
    expected_errors = {
        "request": "response request mismatch", "type": "unexpected response type",
        "path": "commit result mismatch", "size": "commit result mismatch",
        "hash": "commit result mismatch", "session": "commit result mismatch",
        "ack_false": "Invalid CHUNK_ACK", "truncate": "server closed before response",
        "source_changed": "SOURCE_CHANGED",
    }
    with tempfile.TemporaryDirectory(prefix="chunkharbor-cli-peer-") as root:
        source = Path(root) / "source"
        for mode in ["ok", *expected_errors]:
            source.write_bytes(b"abc")
            with socket.socket() as listener:
                listener.settimeout(5)
                listener.bind(("127.0.0.1", 0))
                listener.listen()
                results = queue.Queue()
                thread = threading.Thread(target=peer, args=(listener, mode, source, results),
                                          daemon=True)
                thread.start()
                try:
                    result = subprocess.run([
                        cli, "upload", "--file", str(source), "--path", "safe/file",
                        "--port", str(listener.getsockname()[1])],
                        capture_output=True, text=True, timeout=8)
                finally:
                    thread.join(6)
                assert not thread.is_alive(), (mode, "peer did not finish")
                outcome = results.get_nowait()
                assert outcome is None, (mode, repr(outcome))
                assert result.returncode == (0 if mode == "ok" else 2), (
                    mode, result.returncode, result.stdout, result.stderr)
                assert ("UPLOAD OK" in result.stdout) == (mode == "ok"), (mode, result.stdout)
                if mode == "ok":
                    assert not result.stderr and "sent=3 confirmed=3" in result.stdout
                else:
                    assert expected_errors[mode] in result.stderr, (mode, result.stderr)
                if mode in ("ack_false", "source_changed"):
                    assert "confirmed=3" not in result.stdout
                print("PASS", mode)


if __name__ == "__main__":
    main()
