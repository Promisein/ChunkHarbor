"""Real Windows FilesPage -> SSH tunnel -> private Ubuntu upload server."""
import json
import os
from pathlib import Path
import queue
import re
import shlex
import socket
import subprocess
import sys
import threading

PROBE, HOST, SERVER = sys.argv[1:4]
CLI = SERVER.rsplit('/', 1)[0] + '/chunkharbor-cli'


def ssh(command):
    return subprocess.check_output(['ssh', '-o', 'BatchMode=yes', HOST, command], encoding='utf-8').strip()


root = ssh('mktemp -d /tmp/chunkharbor-m6-gui-files-data-XXXXXXXX')
assert re.fullmatch('/tmp/chunkharbor-m6-gui-files-data-[A-Za-z0-9]{8}', root), root
remote = tunnel = None
pid = None
try:
    remote = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', HOST,
                               'echo $$; exec ' + shlex.quote(SERVER) + ' --upload --root ' + shlex.quote(root) + ' --port 0'],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, encoding='utf-8')
    lines = queue.Queue()
    threading.Thread(target=lambda: [lines.put(line.strip()) for line in remote.stdout], daemon=True).start()
    pid = int(lines.get(timeout=15))
    listening = lines.get(timeout=15)
    assert listening.startswith('LISTENING 127.0.0.1:'), listening
    remote_port = int(listening.rsplit(':', 1)[1])
    # Real old upload API, committed after server startup; no fabricated DB rows.
    code = '''import hashlib,json,pathlib,subprocess,sys,tempfile
cli,port=sys.argv[1:3]
result={}
with tempfile.TemporaryDirectory(prefix="chunkharbor-gui-files-source-") as temp:
    source=pathlib.Path(temp)/"source"
    for name in ["a-中文.txt"]+["file-%03d"%i for i in range(70)]:
        data=(name.encode("utf-8")+b"\\x00\\xff")*3
        source.write_bytes(data)
        subprocess.run([cli,"upload","--port",port,"--path",name,"--file",str(source)],check=True,capture_output=True)
        result[name]={"sha256":hashlib.sha256(data).hexdigest(),"size":str(len(data))}
print(json.dumps(result))
'''
    expected = json.loads(ssh('python3 -c ' + shlex.quote(code) + ' ' + shlex.quote(CLI) + ' ' + str(remote_port)))
    with socket.socket() as local:
        local.bind(('127.0.0.1', 0))
        port = local.getsockname()[1]
    tunnel = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', '-o', 'ExitOnForwardFailure=yes',
                               '-N', '-L', f'{port}:127.0.0.1:{remote_port}', HOST],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    import time
    until = time.monotonic() + 10
    while True:
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=.2): pass
            break
        except OSError:
            assert time.monotonic() < until, 'SSH tunnel did not start'
            time.sleep(.05)
    command = [PROBE, str(port), 'pages']
    if len(sys.argv) > 4: command.append(str(Path(sys.argv[4]).resolve()))
    r = subprocess.run(command, capture_output=True, timeout=20,
                       env=dict(os.environ, QT_QPA_PLATFORM='windows' if os.name == 'nt' and len(sys.argv) > 4 else 'offscreen'))
    results = [line[7:] for line in r.stdout.decode('utf-8').splitlines() if line.startswith('RESULT=')]
    assert r.returncode == 0 and results, (r.returncode, r.stdout, r.stderr)
    try:
        result = json.loads(results[-1])
    except json.JSONDecodeError as error:
        raise AssertionError((len(r.stdout), r.stdout[-600:], r.stderr)) from error
    ordered = sorted(expected, key=lambda x: x.encode('utf-8'))
    assert result['success'] and result['pages'] == [ordered[:32], ordered[32:64], ordered[:32]], result
    assert result['visible'] == 1 and result['next'] and not result['previous'] and not result['restore']
    for entry in result['rows']:
        assert entry['sha256'] == expected[entry['path']]['sha256'] and entry['size'] == expected[entry['path']]['size']
    assert result['heartbeat'] > 0
    print('Windows real Qt file page SSH paging/filter/SHA256 metadata passed; test root=' + root)
finally:
    if tunnel:
        tunnel.terminate()
        tunnel.communicate(timeout=10)
    if pid:
        ssh('kill -TERM ' + str(pid))
    if remote: remote.communicate(timeout=15)
