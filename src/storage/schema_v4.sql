BEGIN IMMEDIATE;
CREATE TABLE directory_snapshots(
    sequence INTEGER PRIMARY KEY AUTOINCREMENT,
    id TEXT NOT NULL UNIQUE,
    root TEXT NOT NULL COLLATE BINARY,
    created_at_s INTEGER NOT NULL CHECK(created_at_s>=0),
    state TEXT NOT NULL CHECK(state IN ('creating','committed')),
    file_count INTEGER NOT NULL CHECK(file_count BETWEEN 0 AND 10000)
);
CREATE INDEX committed_snapshots_by_root_sequence ON directory_snapshots(root,sequence DESC)
    WHERE state='committed';
CREATE TABLE snapshot_entries(
    snapshot_id TEXT NOT NULL REFERENCES directory_snapshots(id),
    relative_path TEXT NOT NULL COLLATE BINARY CHECK(length(relative_path)>0),
    version_id TEXT NOT NULL REFERENCES version_history(version_id),
    version_ref TEXT REFERENCES versions(id),
    legacy_ref TEXT REFERENCES transfers(session_id),
    PRIMARY KEY(snapshot_id,relative_path),
    CHECK((version_ref IS NOT NULL AND version_ref=version_id AND legacy_ref IS NULL)
       OR (legacy_ref IS NOT NULL AND legacy_ref=version_id AND version_ref IS NULL))
) WITHOUT ROWID;
CREATE INDEX snapshot_entries_by_version ON snapshot_entries(version_id);
CREATE INDEX snapshot_entries_by_version_ref ON snapshot_entries(version_ref) WHERE version_ref IS NOT NULL;
CREATE INDEX snapshot_entries_by_legacy_ref ON snapshot_entries(legacy_ref) WHERE legacy_ref IS NOT NULL;
DROP TABLE schema_version;
CREATE TABLE schema_version(version INTEGER NOT NULL CHECK(version=4));
INSERT INTO schema_version VALUES(4);
PRAGMA user_version=4;
COMMIT;
