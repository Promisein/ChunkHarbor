"""M6 live wire tests: use a fresh private root, never user backup data."""
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile

import delta_protocol_test as wire

SERVER = sys.argv[1]
CLI = sys.argv[2]


class ListingPeer(wire.Peer):
    def __init__(self, port, caps=None):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=15)
        self.request_id = 0
        ack = self.ask(1, {'client_name': 'm6-list-test', 'versions': [1],
                          'capabilities': caps if caps is not None else ['file_list_v1']}, 2)
        self.capabilities = ack['capabilities']

    def page(self, after='', limit=64):
        return self.ask(11, {'after_path': after, 'limit': limit}, 12)


def main():
    with tempfile.TemporaryDirectory(prefix='chunkharbor-m6-list-') as temp:
        root = Path(temp) / 'data'
        root.mkdir(mode=0o700)
        server, port = wire.start(root)
        try:
            peer = ListingPeer(port)
            assert 'file_list_v1' in peer.capabilities, peer.capabilities
            assert peer.page() == {'files': [], 'next_after_path': ''}
            # Old upload committed during this process must be visible immediately.
            source = Path(temp) / 'source'
            source.write_bytes(b'old api')
            subprocess.run([CLI, 'upload', '--port', str(port), '--file', str(source),
                            '--path', 'legacy'], check=True, capture_output=True)
            assert peer.page()['files'][0]['path'] == 'legacy'
            with sqlite3.connect(root / 'metadata.sqlite3') as db:
                assert db.execute('SELECT count(*) FROM legacy_versions').fetchone()[0] == 0
            expected = {'legacy': wire.digest(b'old api')}
            for name, data in [('b', b'bbb'), ('a', b''), ('中文.txt', b'utf8')]:
                up = wire.Peer(port)
                wire.upload(up, name, data, [[0, (len(data) + 3) // 4]] if data else [])
                up.close()
                expected[name] = wire.digest(data)
            # Replaced versions must not duplicate the logical path.
            up = wire.Peer(port)
            wire.upload(up, 'b', b'new', [[0, 1]])
            up.close()
            expected['b'] = wire.digest(b'new')
            pending = wire.Peer(port)
            wire.begin(pending, 'unfinished', b'pending')
            pending.close()
            preview = wire.Peer(port)
            wire.begin(preview, 'preview-only', b'', mode='preview')
            preview.close()
            after, entries = '', []
            while True:
                page = peer.page(after, 1)
                assert len(page['files']) == 1
                entries.extend(page['files'])
                after = page['next_after_path']
                if not after:
                    break
                assert after == entries[-1]['path']
            assert [e['path'] for e in entries] == sorted(expected, key=lambda s: s.encode())
            assert {e['path']: e['sha256'] for e in entries} == expected
            assert peer.page('zzzz')['files'][0]['path'] == '中文.txt'
            assert peer.page('龘')['files'] == []
            # A listing must not rebind the active upload session on the same socket.
            mixed = ListingPeer(port, ['upload', 'fixed_block_delta_v1', 'file_list_v1'])
            ready = wire.begin(mixed, 'mixed-empty', b'')
            assert 'mixed-empty' not in [e['path'] for e in mixed.page()['files']]
            wire.manifest(mixed, ready, b'')
            wire.plan(mixed, ready, [])
            mixed.ask(30, {'session_id': ready['session_id']}, 31)
            expected['mixed-empty'] = wire.digest(b'')
            assert 'mixed-empty' in [e['path'] for e in mixed.page()['files']]
            mixed.close()
            # No capability, missing HELLO, malformed controls and oversized counts.
            denied = ListingPeer(port, ['handshake'])
            assert denied.ask(11, {'after_path': '', 'limit': 1}, 15)['code'] == 'UNSUPPORTED_OPERATION'
            denied.close()
            for payload in [{'after_path': '', 'limit': 65}, {'after_path': '../x', 'limit': 1},
                            {'after_path': '', 'limit': 1.5}, b'{"after_path":"","limit":1,"limit":2}']:
                bad = ListingPeer(port)
                assert bad.ask(11, payload, 15)['code'] == 'MALFORMED_MESSAGE'
                bad.close()
            raw = wire.Peer.__new__(wire.Peer)
            raw.sock = socket.create_connection(('127.0.0.1', port), timeout=15)
            raw.request_id = 0
            assert raw.ask(11, {'after_path': '', 'limit': 1}, 15)['code'] == 'HANDSHAKE_REQUIRED'
            raw.close()
            peer.close()
            for index in range(66):
                name = f'count-{index:02d}'
                up = wire.Peer(port)
                wire.upload(up, name, b'', [])
                up.close()
                expected[name] = wire.digest(b'')
            count_peer = ListingPeer(port)
            count_page = count_peer.page()
            assert len(count_page['files']) == 64 and count_page['next_after_path']
            count_peer.close()
            # Real empty committed files exercise byte-limited pages with escaped paths.
            for index in range(20):
                name = f'long-{index:02d}/' + '/'.join(['"' * 240] * 16)
                up = wire.Peer(port)
                wire.upload(up, name, b'', [])
                up.close()
                expected[name] = wire.digest(b'')
            page_peer = ListingPeer(port)
            after, seen, byte_limited = '', {}, False
            while True:
                page = page_peer.page(after)
                assert 0 < len(page['files']) <= 64
                if page['next_after_path']:
                    byte_limited = byte_limited or len(page['files']) < 64
                    assert page['next_after_path'] == page['files'][-1]['path']
                for entry in page['files']:
                    assert entry['path'] not in seen
                    seen[entry['path']] = entry['sha256']
                after = page['next_after_path']
                if not after:
                    break
            assert seen == expected and byte_limited
            page_peer.close()
        finally:
            wire.stop(server)
        server, port = wire.start(root)
        try:
            peer = ListingPeer(port)
            after, seen = '', {}
            while True:
                page = peer.page(after)
                seen.update({e['path']: e['sha256'] for e in page['files']})
                after = page['next_after_path']
                if not after:
                    break
            assert seen == expected
            peer.close()
        finally:
            wire.stop(server)
    print('M6 file list capability, committed-only paging, legacy and restart passed')


if __name__ == '__main__':
    main()
