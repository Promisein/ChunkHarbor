BEGIN IMMEDIATE;
-- Includes committed legacy uploads before they acquire a versions row.
CREATE TABLE version_history(
    sequence INTEGER PRIMARY KEY AUTOINCREMENT,
    version_id TEXT NOT NULL UNIQUE,
    path TEXT NOT NULL,
    committed_at_s INTEGER
);
CREATE INDEX history_by_path_sequence ON version_history(path, sequence DESC);
INSERT INTO version_history(version_id,path)
    SELECT session_id,logical_path FROM transfers WHERE state='COMMITTED' ORDER BY rowid;
-- v2 rowid supplies registration order. An unpublished phase-4 object
-- only qualifies if some other record proves it was already published.
INSERT OR IGNORE INTO version_history(version_id,path)
    SELECT id,path FROM versions WHERE id NOT IN (
        SELECT version_id FROM delta_sessions WHERE phase=4 AND version_id IS NOT NULL
        EXCEPT SELECT version_id FROM delta_sessions WHERE phase=5 AND version_id IS NOT NULL
        EXCEPT SELECT version_id FROM current_files
        EXCEPT SELECT version_id FROM legacy_versions
    ) ORDER BY rowid;
CREATE TRIGGER history_publish_insert AFTER INSERT ON current_files
    WHEN NOT EXISTS(SELECT 1 FROM version_history WHERE version_id=NEW.version_id) BEGIN
    INSERT INTO version_history(version_id,path,committed_at_s)
    VALUES(NEW.version_id,NEW.path,CAST(strftime('%s','now') AS INTEGER));
END;
CREATE TRIGGER history_publish_update AFTER UPDATE OF version_id ON current_files
    WHEN NOT EXISTS(SELECT 1 FROM version_history WHERE version_id=NEW.version_id) BEGIN
    INSERT INTO version_history(version_id,path,committed_at_s)
    VALUES(NEW.version_id,NEW.path,CAST(strftime('%s','now') AS INTEGER));
END;
CREATE TRIGGER history_legacy_commit AFTER UPDATE OF state ON transfers
    WHEN NEW.state='COMMITTED' AND OLD.state<>'COMMITTED'
         AND NOT EXISTS(SELECT 1 FROM version_history WHERE version_id=NEW.session_id) BEGIN
    INSERT INTO version_history(version_id,path,committed_at_s)
    VALUES(NEW.session_id,NEW.logical_path,CAST(strftime('%s','now') AS INTEGER));
END;
DROP TABLE schema_version;
CREATE TABLE schema_version(version INTEGER NOT NULL CHECK(version=3));
INSERT INTO schema_version VALUES(3);
PRAGMA user_version=3;
COMMIT;
