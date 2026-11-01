"""Schema-only constraints use private in-memory databases, never user storage."""
import sqlite3
from pathlib import Path
root = Path(__file__).resolve().parents[2]
v1 = (root / 'src/storage/schema_v1.sql').read_text()
v2 = (root / 'src/storage/schema_v2.sql').read_text()
db = sqlite3.connect(':memory:')
db.executescript(v1)
db.executescript(v2)
assert db.execute('pragma user_version').fetchone()[0] == 2
assert db.execute('select version from schema_version').fetchone()[0] == 2
for identity, digest in [('a'*32,'1'*64),('b'*32,'2'*64),('c'*32,'1'*64)]:
    db.execute('insert into versions values(?,?,?,?,?)',(identity,'file',4,4,digest))
db.execute('insert into current_files values(?,?)',('file','a'*32))
assert db.execute('update current_files set version_id=? where path=? and version_id=?',('b'*32,'file','a'*32)).rowcount == 1
assert db.execute('update current_files set version_id=? where path=? and version_id=?',('c'*32,'file','b'*32)).rowcount == 1
assert db.execute('update current_files set version_id=? where path=? and version_id=?',('b'*32,'file','a'*32)).rowcount == 0
assert db.execute('select version_id from current_files').fetchone()[0] == 'c'*32
try:
    db.execute('insert into current_files values(?,?)',('bad','d'*32))
except sqlite3.IntegrityError:
    pass
else:
    raise AssertionError('foreign key accepted')
broken = sqlite3.connect(':memory:')
broken.executescript(v1)
try:
    broken.executescript(v2.replace('PRAGMA user_version=2;', 'SELECT missing_column FROM missing_table;'))
except sqlite3.OperationalError:
    broken.rollback()
else:
    raise AssertionError('fault injection did not fail')
assert broken.execute('pragma user_version').fetchone()[0] == 1
assert broken.execute('select version from schema_version').fetchone()[0] == 1
assert not broken.execute("select name from sqlite_master where name='versions'").fetchall()
print('schema v2 migration, CAS ABA, foreign key and rollback passed')

db.execute("insert into delta_sessions(id,request,logical_path,mode,phase,created) values('one','{}','same',0,0,0)")
try:
    db.execute("insert into delta_sessions(id,request,logical_path,mode,phase,created) values('two','{}','same',0,0,0)")
except sqlite3.IntegrityError:
    pass
else:
    raise AssertionError('two active uploads for one path')
db.execute("insert into delta_sessions(id,request,logical_path,mode,phase,created) values('preview','{}','same',1,0,0)")
