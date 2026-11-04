// Included by UploadStorage.cpp after DeltaStorage.inc, sharing pinned root FDs,
// safe open/hash helpers, and the database mutex. All methods run on disk workers.
struct DownloadSession {
    const void *owner = nullptr;
    protocol::DownloadReady ready;
    UniqueFd data;
    std::mutex mutex;
};
OpenedDownload UploadStorage::openDownload(const protocol::BeginDownload &request,
                                           protocol::DownloadSelection selection) {
    const bool historical = selection == protocol::DownloadSelection::Historical;
    if (historical)
        protocol::encodeBeginHistoricalDownload(request);
    else
        protocol::encodeBeginDownload(request);
    auto &i = *impl_;
    VersionRecord version;
    bool immutable = false;
    {
        std::lock_guard<std::mutex> guard(i.dbMutex);
        if (historical) {
            // Only published ledger entries qualify. Legacy commits have their own
            // immutable object even before the versions table is populated.
            DStmt q(i.db->nativeHandle(),
                    "SELECT COALESCE(v.size,t.total_size,-1),COALESCE(v.sha256,t.expected_sha256,'') "
                    "FROM version_history h LEFT JOIN versions v ON v.id=h.version_id AND v.path=h.path "
                    "LEFT JOIN transfers t ON t.session_id=h.version_id AND t.logical_path=h.path "
                    "AND t.state='COMMITTED' WHERE h.version_id=? AND h.path=?");
            q.text(1, request.versionId).text(2, request.path);
            if (!q.row()) fail("VERSION_NOT_FOUND", "No published version at the requested path");
            const auto size = q.number(0);
            if (size < 0 || static_cast<std::uint64_t>(size) > protocol::MaxDownloadFileSize)
                fail("VERSION_UNAVAILABLE", "Historical size metadata is unavailable or invalid");
            version = {request.versionId, request.path, static_cast<std::uint64_t>(size), 0, q.str(1)};
            immutable = true;
        } else if (auto current = i.db->currentVersion(request.path)) {
            version = *current;
            immutable = true;
        } else {
            DStmt q(i.db->nativeHandle(), "SELECT session_id,total_size,chunk_size,expected_sha256 FROM transfers "
                         "WHERE logical_path=? AND state='COMMITTED' LIMIT 1");
            q.text(1, request.path);
            if (!q.row()) fail("FILE_NOT_FOUND", "No committed file at path");
            version = {q.str(0), request.path, static_cast<std::uint64_t>(q.number(1)),
                       static_cast<std::uint32_t>(q.number(2)), q.str(3)};
        }
    }
    if (!request.versionId.empty() && request.versionId != version.id)
        fail("VERSION_CONFLICT", "Current file version changed");
    auto session = std::make_shared<DownloadSession>();
    session->owner = impl_.get();
    session->ready = {request.path, version.id, version.size, version.sha256};
    try {
        protocol::encodeDownloadReady(session->ready);
        // Open existing paths only. An exact historical request never reads files/.
        session->data = immutable
            ? regularFile(i.root.get(), ".versions/" + version.id + "/data", O_RDONLY)
            : regularFile(i.files.get(), request.path, O_RDONLY);
    } catch (const protocol::UploadFailure &) {
        if (historical)
            fail("VERSION_UNAVAILABLE", "Historical metadata or immutable object is unavailable");
        throw;
    }
    struct stat st{};
    io(::fstat(session->data.get(), &st) == 0);
    if (st.st_size < 0 || static_cast<std::uint64_t>(st.st_size) != version.size ||
        hashFd(session->data.get(), version.size) != version.sha256)
        fail("CHECKSUM_MISMATCH", "Committed file failed verification");
    return {session, session->ready};
}
protocol::DownloadChunk UploadStorage::readDownload(const DownloadSessionPtr &session,
                                                    const protocol::GetDownloadChunk &request) {
    protocol::encodeGetDownloadChunk(request);
    if (!session || session->owner != impl_.get())
        fail("SESSION_NOT_FOUND", "No download session");
    std::lock_guard<std::mutex> guard(session->mutex);
    if (request.versionId != session->ready.versionId)
        fail("VERSION_CONFLICT", "Download version mismatch");
    if (request.offset >= session->ready.size || request.length > session->ready.size - request.offset)
        fail("INVALID_RANGE", "Download range exceeds file size");
    protocol::DownloadChunk result{request.versionId, request.offset, {}, std::string(request.length, '\0')};
    std::size_t done = 0;
    while (done < result.data.size()) {
        auto n = ::pread(session->data.get(), result.data.data() + done, result.data.size() - done,
                         static_cast<off_t>(request.offset + done));
        if (n < 0 && errno == EINTR) continue;
        io(n > 0);
        done += static_cast<std::size_t>(n);
    }
    result.sha256 = sha256(result.data);
    return result;
}
