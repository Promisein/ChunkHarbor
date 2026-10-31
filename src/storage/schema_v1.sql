-- M3 schema v1. Apply as a single transaction; application sets WAL and FULL sync.
PRAGMA foreign_keys = ON;
BEGIN IMMEDIATE;
CREATE TABLE schema_version (version INTEGER NOT NULL CHECK (version = 1));
INSERT INTO schema_version VALUES (1);
CREATE TABLE transfers (
  session_id TEXT PRIMARY KEY CHECK (length(session_id) = 32 AND session_id NOT GLOB '*[^0-9a-f]*'),
  logical_path TEXT NOT NULL CHECK (length(logical_path) > 0),
  expected_sha256 TEXT NOT NULL CHECK (length(expected_sha256) = 64 AND expected_sha256 NOT GLOB '*[^0-9a-f]*'),
  total_size INTEGER NOT NULL CHECK (total_size BETWEEN 0 AND 67108864),
  chunk_size INTEGER NOT NULL CHECK (chunk_size BETWEEN 1 AND 1048576),
  mtime_ns INTEGER NOT NULL DEFAULT 0,
  temp_path TEXT NOT NULL DEFAULT '',
  state TEXT NOT NULL CHECK (state IN ('CREATED','RECEIVING','VERIFYING','COMMITTING','COMMITTED','FAILED','EXPIRED')),
  created_at_ns INTEGER NOT NULL,
  updated_at_ns INTEGER NOT NULL,
  last_error TEXT,
  published_sha256 TEXT CHECK (published_sha256 IS NULL OR length(published_sha256) = 64),
  CHECK (updated_at_ns >= created_at_ns)
);
CREATE UNIQUE INDEX one_active_transfer_per_path ON transfers(logical_path)
  WHERE state IN ('CREATED','RECEIVING','VERIFYING','COMMITTING');
CREATE TABLE transfer_chunks (
  session_id TEXT NOT NULL REFERENCES transfers(session_id) ON DELETE CASCADE,
  chunk_index INTEGER NOT NULL CHECK (chunk_index >= 0 AND chunk_index < 65536),
  sha256 TEXT NOT NULL CHECK (length(sha256) = 64 AND sha256 NOT GLOB '*[^0-9a-f]*'),
  durable_at_ns INTEGER NOT NULL,
  PRIMARY KEY (session_id, chunk_index)
) WITHOUT ROWID;
CREATE INDEX transfer_chunks_by_session ON transfer_chunks(session_id);
PRAGMA user_version = 1;
COMMIT;
