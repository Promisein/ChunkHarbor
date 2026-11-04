namespace {
std::string snapshotText(sqlite3_stmt *stmt, int column) {
    const auto *value = sqlite3_column_text(stmt, column);
    if (!value) throw std::runtime_error("Missing snapshot metadata");
    return {reinterpret_cast<const char *>(value),
            static_cast<std::size_t>(sqlite3_column_bytes(stmt, column))};
}
protocol::SnapshotInfo snapshotHeader(sqlite3_stmt *stmt) {
    return {snapshotText(stmt, 0), snapshotText(stmt, 1),
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 2)),
            static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 3)),
            static_cast<std::uint32_t>(sqlite3_column_int(stmt, 4))};
}
}

protocol::SnapshotInfo SqliteStore::createSnapshot(const std::string &id, const std::string &root) {
    protocol::encodeGetSnapshot(id);
    protocol::encodeCreateSnapshot(root);
    exec(db_, "BEGIN IMMEDIATE");
    try {
        Statement create(db_, "INSERT INTO directory_snapshots(id,root,created_at_s,state,file_count) "
                              "VALUES(?1,?2,CAST(strftime('%s','now') AS INTEGER),'creating',0)");
        bindText(create.value, 1, id);
        bindText(create.value, 2, root);
        done(db_, create.value);
        // A BINARY range covers precisely root + '/' and its descendants.
        // Keep candidate rows with broken references so constraints abort the whole capture.
        Statement capture(db_,
            "WITH candidates(path,id,version_ref,legacy_ref) AS ("
            " SELECT c.path,c.version_id,v.id,NULL FROM current_files c "
            " LEFT JOIN versions v ON v.id=c.version_id AND v.path=c.path "
            " AND v.size BETWEEN 0 AND ?6 AND length(v.sha256)=64 AND v.sha256 NOT GLOB '*[^0-9a-f]*' "
            " WHERE c.path>=?3 AND c.path<?4 "
            " UNION ALL SELECT t.logical_path,t.session_id,NULL,CASE "
            " WHEN t.total_size BETWEEN 0 AND ?6 AND length(t.expected_sha256)=64 "
            " AND t.expected_sha256 NOT GLOB '*[^0-9a-f]*' THEN t.session_id END FROM transfers t "
            " WHERE t.state='COMMITTED' AND t.logical_path>=?3 AND t.logical_path<?4 "
            " AND NOT EXISTS(SELECT 1 FROM current_files c WHERE c.path=t.logical_path) LIMIT ?5) "
            "INSERT INTO snapshot_entries(snapshot_id,relative_path,version_id,version_ref,legacy_ref) "
            "SELECT ?1,substr(c.path,length(?2)+2),h.version_id,c.version_ref,c.legacy_ref "
            "FROM candidates c LEFT JOIN version_history h ON h.version_id=c.id AND h.path=c.path");
        bindText(capture.value, 1, id);
        bindText(capture.value, 2, root);
        bindText(capture.value, 3, root + '/');
        bindText(capture.value, 4, root + '0');
        sqlite3_bind_int(capture.value, 5, protocol::MaxSnapshotFiles + 1);
        sqlite3_bind_int64(capture.value, 6, protocol::MaxDownloadFileSize);
        done(db_, capture.value);
        const int count = sqlite3_changes(db_);
        if (count > static_cast<int>(protocol::MaxSnapshotFiles))
            throw protocol::UploadFailure("QUOTA_EXCEEDED", "Snapshot exceeds file count limit");

        Statement publish(db_, "UPDATE directory_snapshots SET state='committed',file_count=?2 WHERE id=?1");
        bindText(publish.value, 1, id);
        sqlite3_bind_int(publish.value, 2, count);
        done(db_, publish.value);
        auto result = getSnapshot(id);
        protocol::encodeSnapshotInfo(result);
        exec(db_, "COMMIT");
        return result;
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

protocol::SnapshotInfo SqliteStore::getSnapshot(const std::string &id) const {
    protocol::encodeGetSnapshot(id);
    Statement query(db_, "SELECT id,root,sequence,created_at_s,file_count FROM directory_snapshots "
                         "WHERE id=?1 AND state='committed'");
    bindText(query.value, 1, id);
    const auto status = sqlite3_step(query.value);
    if (status == SQLITE_DONE)
        throw protocol::UploadFailure("SNAPSHOT_NOT_FOUND", "No committed snapshot with that ID");
    if (status != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(db_));
    return snapshotHeader(query.value);
}

protocol::SnapshotsPage SqliteStore::listSnapshots(const protocol::ListSnapshots &request) const {
    protocol::encodeListSnapshots(request);
    Statement query(db_, "SELECT id,root,sequence,created_at_s,file_count FROM directory_snapshots "
                         "WHERE root=?1 AND state='committed' AND sequence<=?2 ORDER BY sequence DESC LIMIT ?3");
    bindText(query.value, 1, request.root);
    sqlite3_bind_int64(query.value, 2, request.before ? static_cast<sqlite3_int64>(*request.before - 1) : INT64_MAX);
    sqlite3_bind_int(query.value, 3, static_cast<int>(request.limit + 1));
    protocol::SnapshotsPage result{request.root, {}, {}};
    int status;
    while ((status = sqlite3_step(query.value)) == SQLITE_ROW)
        result.snapshots.push_back(snapshotHeader(query.value));
    if (status != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
    if (result.snapshots.size() > request.limit) {
        result.snapshots.pop_back();
        result.nextBefore = result.snapshots.back().sequence;
    }
    return result;
}

protocol::SnapshotEntriesPage SqliteStore::listSnapshotEntries(const protocol::ListSnapshotEntries &request) const {
    protocol::encodeListSnapshotEntries(request);
    getSnapshot(request.snapshotId);
    Statement query(db_, "SELECT e.relative_path,e.version_id,COALESCE(v.size,t.total_size),"
                         "COALESCE(v.sha256,t.expected_sha256) FROM snapshot_entries e "
                         "LEFT JOIN versions v ON v.id=e.version_ref "
                         "LEFT JOIN transfers t ON t.session_id=e.legacy_ref "
                         "WHERE e.snapshot_id=?1 AND e.relative_path>?2 ORDER BY e.relative_path LIMIT ?3");
    bindText(query.value, 1, request.snapshotId);
    bindText(query.value, 2, request.afterPath);
    sqlite3_bind_int(query.value, 3, static_cast<int>(request.limit + 1));
    protocol::SnapshotEntriesPage result{request.snapshotId, {}, {}};
    int status;
    while ((status = sqlite3_step(query.value)) == SQLITE_ROW)
        result.entries.push_back({snapshotText(query.value, 0), snapshotText(query.value, 1),
            static_cast<std::uint64_t>(sqlite3_column_int64(query.value, 2)), snapshotText(query.value, 3)});
    if (status != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
    if (result.entries.size() > request.limit) {
        result.entries.pop_back();
        result.nextAfterPath = result.entries.back().path;
    }
    return result;
}
