#include "chunkharbor/client_core/DeltaExchange.h"
#include "chunkharbor/protocol/ChunkAck.h"
#include "chunkharbor/protocol/SessionStatus.h"
#include "chunkharbor/protocol/UploadControl.h"
#include <algorithm>
namespace chunkharbor::client {
using namespace protocol;
DeltaOutcome runDelta(SourceSnapshot &source, const std::function<Frame(const Frame &)> &transport,
                      const DeltaOptions &options) {
    std::uint64_t requestId = 2;
    auto request = [&](std::uint16_t type, std::string body, std::uint16_t expected = 0) {
        if (options.cancelled())
            throw std::runtime_error("cancelled; resumable state retained");
        Frame q{type, requestId++, std::move(body)};
        auto r = transport(q);
        if (r.requestId != q.requestId)
            throw std::runtime_error("response ID mismatch");
        if (r.type == 15) {
            auto e = decodeUploadError(r.payload);
            throw UploadFailure(e.code, e.message);
        }
        if (expected && r.type != expected)
            throw std::runtime_error("unexpected response type");
        return r;
    };
    source.verify();
    DeltaCheckpoint cp;
    DeltaPhase phase = DeltaPhase::Collecting;
    std::uint32_t nextPage = 0;
    std::string bitmap;
    const auto &m = source.metadata;
    if (!m.chunkSize || m.chunkSize > MaxChunkDataSize || m.size > 64ULL * 1024 * 1024 ||
        (m.size + m.chunkSize - 1) / m.chunkSize > 65536)
        throw std::runtime_error("invalid source geometry");
    const auto blocks = static_cast<std::uint32_t>((m.size + m.chunkSize - 1) / m.chunkSize);
    DeltaOutcome outcome;
    auto checkResult = [&](const DeltaResult &r) {
        if (r.sessionId != cp.session || r.path != m.path || r.size != m.size ||
            r.sha256 != m.sha256 || !r.committed)
            throw std::runtime_error("delta result mismatch");
    };
    if (options.resume) {
        cp = *options.resume;
        if ((options.base.kind == BaseSelector::Kind::Absent && cp.base) ||
            (options.base.kind == BaseSelector::Kind::Exact &&
             cp.base != std::optional<VersionId>(options.base.versionId)))
            throw std::runtime_error("resume baseline selector mismatch");
        auto s = decodeDeltaStatus(request(28, encodeQueryDelta({cp.session}), 29).payload);
        if (s.sessionId != cp.session || s.mode != options.mode || s.baseVersion != cp.base)
            throw std::runtime_error("resume session mismatch");
        phase = s.phase;
        nextPage = s.nextManifestPage;
        bitmap = s.missingBitmap;
        if (phase == DeltaPhase::Committed) {
            if (!s.result)
                throw std::runtime_error("missing committed result");
            checkResult(*s.result);
            outcome.result = s.result;
            return outcome;
        }
    } else {
        auto r = decodeDeltaReady(
            request(24, encodeBeginDelta({m, options.mode, options.base}), 25).payload);
        if (r.blockCount != blocks)
            throw std::runtime_error("ready geometry mismatch");
        if (options.base.kind == BaseSelector::Kind::Absent && r.baseVersion)
            throw std::runtime_error("unexpected baseline");
        if (options.base.kind == BaseSelector::Kind::Exact &&
            r.baseVersion != std::optional<VersionId>(options.base.versionId))
            throw std::runtime_error("baseline mismatch");
        cp = {r.sessionId, r.baseVersion};
        if (!options.save)
            throw std::runtime_error("checkpoint persistence required");
        options.save(cp);
    }
    if (phase == DeltaPhase::Collecting) {
        ManifestReader reader(source, cp.session);
        std::uint32_t pages = 0;
        while (auto p = reader.next()) {
            ++pages;
            if (p->pageIndex < nextPage)
                continue;
            auto ack = decodeManifestAck(request(18, encodeManifestPage(*p), 26).payload);
            if (ack.sessionId != cp.session || ack.nextPageIndex != p->pageIndex + 1 ||
                ack.phase != DeltaPhase::Collecting)
                throw std::runtime_error("manifest ack mismatch");
        }
        if (nextPage > pages)
            throw std::runtime_error("manifest resume page mismatch");
        auto ack = decodeManifestAck(
            request(19, encodeManifestDone({cp.session, pages, blocks}), 26).payload);
        if (ack.sessionId != cp.session || ack.nextPageIndex != pages ||
            ack.phase != DeltaPhase::PlanReady)
            throw std::runtime_error("manifest done mismatch");
        phase = DeltaPhase::PlanReady;
    }
    std::vector<bool> missing(blocks, false);
    if (phase == DeltaPhase::PlanReady) {
        PlanValidator validator(cp.session, cp.base, m.size, m.chunkSize);
        for (std::uint32_t p = 0;; ++p) {
            if (p > 64)
                throw std::runtime_error("too many plan pages");
            auto r = request(27, encodeGetPlanPage({cp.session, p}));
            if (r.type == 21) {
                outcome.plan = decodePlanDone(r.payload);
                validator.finish(outcome.plan);
                break;
            }
            if (r.type != 20)
                throw std::runtime_error("missing plan DONE");
            auto page = decodePlanPage(r.payload);
            validator.append(page);
            for (auto range : page.missingRanges)
                for (auto i = range.begin; i < range.end; ++i)
                    missing.at(i) = true;
        }
    } else if (phase == DeltaPhase::Transferring || phase == DeltaPhase::Verifying) {
        const auto bytes = (blocks + 7) / 8;
        if (bitmap.size() != bytes * 2)
            throw std::runtime_error("missing bitmap length mismatch");
        const std::string digits = "0123456789abcdef";
        for (std::uint32_t b = 0; b < bytes; ++b) {
            auto hi = digits.find(bitmap[b * 2]), lo = digits.find(bitmap[b * 2 + 1]);
            if (hi == std::string::npos || lo == std::string::npos)
                throw std::runtime_error("invalid missing bitmap");
            auto value = (hi << 4) | lo;
            for (unsigned bit = 0; bit < 8; ++bit) {
                auto i = b * 8 + bit;
                if (i < blocks)
                    missing[i] = (value & (1U << bit)) != 0;
                else if (value & (1U << bit))
                    throw std::runtime_error("bitmap padding set");
            }
        }
    } else
        throw std::runtime_error("delta phase requires retry or is terminal");
    if (phase == DeltaPhase::Transferring || phase == DeltaPhase::Verifying) {
        outcome.plan.sessionId = cp.session;
        outcome.plan.baseVersion = cp.base;
        outcome.plan.blockCount = blocks;
        for (std::uint32_t i = 0; i < blocks; ++i) {
            auto length =
                std::min<std::uint64_t>(m.chunkSize, m.size - std::uint64_t(i) * m.chunkSize);
            if (missing[i]) {
                ++outcome.plan.missingBlocks;
                outcome.plan.missingBytes += length;
            } else {
                ++outcome.plan.reusedBlocks;
                outcome.plan.reusedBytes += length;
            }
        }
    }
    if (options.mode == DeltaMode::Preview) {
        source.verify();
        return outcome;
    }
    source.verify();
    for (std::uint32_t i = 0; i < blocks; ++i)
        if (missing[i]) {
            auto offset = std::uint64_t(i) * m.chunkSize;
            auto length = std::min<std::uint64_t>(m.chunkSize, m.size - offset);
            auto bytes = source.read(offset, static_cast<std::size_t>(length));
            if (bytes.size() != length)
                throw std::runtime_error("short source read");
            DataChunk chunk{cp.session, offset, source.hash(bytes), std::move(bytes)};
            auto ack = decodeChunkAck(request(5, encodeDataChunk(chunk), 6).payload);
            validateChunkAckForChunk(ack, chunk);
            ++outcome.sentBlocks;
            outcome.acknowledgedBytes += length;
        }
    source.verify();
    auto result = decodeDeltaResult(request(30, encodeCommitDelta({cp.session}), 31).payload);
    checkResult(result);
    outcome.result = result;
    return outcome;
}
} // namespace chunkharbor::client
