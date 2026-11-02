"""A/B rescan regression on real Windows worker/controller and private Ubuntu data."""
import hashlib,json,os,queue,re,shlex,shutil,socket,subprocess,sys,tempfile,threading,time
from pathlib import Path
PROBE,HOST,SERVER=sys.argv[1:4]
def ssh(command):
    return subprocess.check_output(['ssh','-o','BatchMode=yes',HOST,command],encoding='utf-8').strip()
root=ssh('mktemp -d /home/ning/ChunkHarbor/build/rescan-data-XXXXXXXX')
assert re.fullmatch('/home/ning/ChunkHarbor/build/rescan-data-[A-Za-z0-9]{8}',root)
remote=tunnel=None
pid=None
try:
    remote=subprocess.Popen(['ssh','-o','BatchMode=yes',HOST,'echo $$; exec '+shlex.quote(SERVER)+' --upload --root '+root+' --port 0'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,encoding='utf-8')
    lines=queue.Queue()
    def consume():
        for line in remote.stdout: lines.put(line.strip())
    threading.Thread(target=consume,daemon=True).start()
    pid=int(lines.get(timeout=15)); listening=lines.get(timeout=15)
    assert listening.startswith('LISTENING 127.0.0.1:')
    remote_port=int(listening.rsplit(':',1)[1])
    with socket.socket() as sock:
        sock.bind(('127.0.0.1',0)); port=sock.getsockname()[1]
    tunnel=subprocess.Popen(['ssh','-o','BatchMode=yes','-o','ExitOnForwardFailure=yes','-N','-L',f'{port}:127.0.0.1:{remote_port}',HOST])
    until=time.monotonic()+10
    while True:
        try:
            with socket.create_connection(('127.0.0.1',port),timeout=.2): pass
            break
        except OSError:
            assert time.monotonic()<until
            time.sleep(.05)
    with tempfile.TemporaryDirectory(prefix='chunkharbor-rescan-') as temp:
        local=Path(temp); a=local/'A'; b=local/'B'; a.mkdir(); b.mkdir()
        (a/'000-completed.txt').write_bytes(b'completed')
        (a/'source.bin').write_bytes(os.urandom(4*1024*1024))
        (b/'other.txt').write_bytes(b'other directory')
        def run(source,state,prefix,operation='backup',mode='full',ok=True,extra=None):
            r=subprocess.run([PROBE,str(local/state),str(source),str(port),prefix,operation,mode]+([str(extra)] if extra else []),capture_output=True,timeout=95,env=dict(os.environ,QT_QPA_PLATFORM='offscreen'))
            data=[line[7:] for line in r.stdout.decode('utf-8').splitlines() if line.startswith('RESULT=')]
            assert data,(r.returncode,r.stdout,r.stderr)
            result=json.loads(data[-1])
            assert result['success']==ok and (r.returncode==0)==ok,result
            return result
        def verify(source,prefix):
            for f in source.iterdir():
                actual=ssh('sha256sum -- '+shlex.quote(root+'/files/'+prefix+'/'+f.name)).split()[0]
                assert actual==hashlib.sha256(f.read_bytes()).hexdigest()
        # Completed own first/full files are safely recognizable on a fresh preview.
        run(a,'complete','complete')
        again=run(a,'complete','complete','preview')
        assert all(row['change']=='未变化' and row['blocks']=='0' for row in again['rows']),again
        # A foreign first/full upload still cannot overwrite an existing target.
        run(a,'foreign','complete','preview',ok=False)
        verify(a,'complete')
        alternate=local/'alternate'; shutil.copytree(a,alternate)
        alternate_file=alternate/'source.bin'
        changed=bytearray(alternate_file.read_bytes()); changed[0]^=1; alternate_file.write_bytes(changed)
        run(alternate,'external','complete',mode='incremental')
        run(a,'complete','complete','preview',ok=False)
        verify(alternate,'complete')
        print('PASS completed full rescan and foreign/changed-target protection',flush=True)
        # Cancel -> preview A -> preview B -> preview A; upload B then resume A.
        run(a,'switch','switch','cancel-large',ok=False)
        before={p.name:p.read_bytes() for p in (local/'switch'/'sessions').glob('*.json')}
        run(a,'switch','switch','preview')
        switched=run(a,'switch','switch','switch-preview',extra=b)
        assert all(not row['path'].endswith('other.txt') for row in switched['rows'])
        run(b,'switch','switch','preview')
        run(a,'switch','switch','preview')
        run(b,'switch','switch')
        run(a,'switch','switch',mode='resume')
        verify(a,'switch'); verify(b,'switch')
        for _ in range(3):
            run(b,'switch','switch','preview')
            run(a,'switch','switch','preview',mode='resume')
        run(a,'switch','switch','rapid-preview',mode='resume')
        run(a,'scan-cancel','scan-cancel','scan-cancel',ok=False)
        run(a,'scan-cancel','scan-cancel','preview')
        empty=local/'empty-source'; empty.mkdir()
        assert not run(empty,'empty-state','empty','preview')['rows']
        run(local/'missing-source','missing-state','missing','preview',ok=False)
        if os.name=='nt':
            import ctypes
            kernel=ctypes.WinDLL('kernel32',use_last_error=True)
            kernel.CreateFileW.restype=ctypes.c_void_p
            kernel.CreateFileW.argtypes=[ctypes.c_wchar_p,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_void_p,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_void_p]
            kernel.CloseHandle.argtypes=[ctypes.c_void_p]
            locked=kernel.CreateFileW(str(a/'source.bin'),0x80000000,0,None,3,0,None)
            assert locked not in (None,ctypes.c_void_p(-1).value)
            try: run(a,'locked','locked','preview',ok=False)
            finally: kernel.CloseHandle(locked)
            run(a,'locked','locked','preview')
        print('PASS A/B switching, resume, rapid requests, scan cancellation and source boundaries',flush=True)
        # Incomplete changed sources must never blindly resume old acknowledged bytes.
        run(a,'changed','changed','cancel-large',ok=False)
        pending=[p for p in (local/'changed'/'sessions').glob('*.json') if not json.loads(p.read_text()).get('completed')]
        assert pending
        entry=json.loads(pending[0].read_text()); original=Path(entry['identity']['local'])
        preserved=pending[0].read_bytes(); content=original.read_bytes()
        original.write_bytes(content+b'changed')
        run(a,'changed','changed','preview',mode='resume',ok=False)
        assert pending[0].read_bytes()==preserved
        # A different destination is a separate task; the old resumable state remains.
        run(a,'changed','changed-new',mode='incremental')
        verify(a,'changed-new')
        original.write_bytes(content)
        run(a,'changed','changed',mode='resume')
        verify(a,'changed')
        print('rescan complete/cancel/A-B-A/B-upload/A-resume/changed-source preservation passed; root='+root)
finally:
    if tunnel: tunnel.terminate(); tunnel.wait(timeout=10)
    if pid: ssh('kill -TERM '+str(pid))
    if remote: remote.communicate(timeout=15)
    cleanup="import pathlib,re,shutil; p=pathlib.Path("+repr(root)+"); assert p.parent==pathlib.Path('/home/ning/ChunkHarbor/build') and re.fullmatch(r'rescan-data-[A-Za-z0-9]{8}',p.name) and not p.is_symlink(); shutil.rmtree(p)"
    ssh('python3 -c '+shlex.quote(cleanup))
