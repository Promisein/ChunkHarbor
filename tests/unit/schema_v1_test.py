import pathlib
import sqlite3
import sys
import tempfile

schema = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
with tempfile.TemporaryDirectory() as temp:
    db = sqlite3.connect(pathlib.Path(temp) / "metadata.sqlite3")
    db.execute("PRAGMA journal_mode=WAL")
    db.execute("PRAGMA synchronous=FULL")
    db.executescript(schema)
    assert db.execute("PRAGMA integrity_check").fetchone() == ("ok",)
    assert db.execute("SELECT version FROM schema_version").fetchone() == (1,)
    sid = "a" * 32
    digest = "b" * 64
    row = (sid, "docs/a.txt", digest, 5, 4, "RECEIVING", 1, 1)
    db.execute("INSERT INTO transfers(session_id,logical_path,expected_sha256,total_size,chunk_size,state,created_at_ns,updated_at_ns) VALUES (?,?,?,?,?,?,?,?)", row)
    db.execute("INSERT INTO transfer_chunks VALUES (?,?,?,?)", (sid, 0, digest, 2))
    db.commit()
    db.close()
    db = sqlite3.connect(pathlib.Path(temp) / "metadata.sqlite3")
    db.execute("PRAGMA foreign_keys=ON")
    assert db.execute("SELECT chunk_index FROM transfer_chunks WHERE session_id=?", (sid,)).fetchall() == [(0,)]
    for sql, params in [
        ("INSERT INTO transfer_chunks VALUES (?,?,?,?)", ("c" * 32, 0, digest, 2)),
        ("INSERT INTO transfer_chunks VALUES (?,?,?,?)", (sid, 0, digest, 2)),
        ("INSERT INTO transfers(session_id,logical_path,expected_sha256,total_size,chunk_size,state,created_at_ns,updated_at_ns) VALUES (?,?,?,?,?,?,?,?)", ("c" * 32, "docs/a.txt", digest, 5, 4, "CREATED", 1, 1)),
    ]:
        try:
            db.execute(sql, params)
            raise AssertionError("constraint was not enforced")
        except sqlite3.IntegrityError:
            pass
    db.execute("UPDATE transfers SET state='COMMITTED' WHERE session_id=?", (sid,))
    db.execute("INSERT INTO transfers(session_id,logical_path,expected_sha256,total_size,chunk_size,state,created_at_ns,updated_at_ns) VALUES (?,?,?,?,?,?,?,?)", ("c" * 32, "docs/a.txt", digest, 5, 4, "CREATED", 1, 1))
    db.commit()
    db.close()
print("schema_v1_test passed")
