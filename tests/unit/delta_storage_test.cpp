#include "chunkharbor/common/Sha256.h"
#include "chunkharbor/storage/DeltaStorage.h"
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static bool killAfterPublish = false;
extern "C" int __real_renameat(int, const char *, int, const char *);
extern "C" int __wrap_renameat(int a, const char *b, int c, const char *d) {
    int r = __real_renameat(a, b, c, d);
    if (r == 0 && killAfterPublish && std::strcmp(d, "file") == 0)
        ::kill(::getpid(), SIGKILL);
    return r;
}
using namespace chunkharbor;
void require(bool b) {
    if (!b)
        throw std::runtime_error("delta storage assertion");
}
template <class F> void rejected(F f) {
    bool bad = false;
    try {
        f();
    } catch (const std::exception &) {
        bad = true;
    }
    require(bad);
}
protocol::BeginDelta request(const std::string &bytes,
                             protocol::DeltaMode mode = protocol::DeltaMode::Upload) {
    protocol::BeginDelta b;
    b.metadata = {"test/file", bytes.size(), 0, 4, toHex(sha256(bytes))};
    b.mode = mode;
    return b;
}
protocol::SessionId manifest(DeltaStorage &d, const protocol::BeginDelta &b,
                             const std::string &bytes) {
    auto ready = d.begin(b);
    protocol::ManifestPage p;
    p.sessionId = ready.sessionId;
    for (std::size_t off = 0; off < bytes.size(); off += 4)
        p.hashes.push_back(sha256(bytes.substr(off, 4)));
    if (!p.hashes.empty()) {
        auto a = d.appendManifest(p);
        require(a.nextPageIndex == 1);
        require(d.appendManifest(p).nextPageIndex == 1);
        auto conflict = p;
        conflict.hashes[0][0] ^= 1;
        rejected([&] { d.appendManifest(conflict); });
    }
    d.finishManifest({ready.sessionId, p.hashes.empty() ? 0U : 1U, ready.blockCount});
    return ready.sessionId;
}
protocol::DeltaResult upload(DeltaStorage &d, const std::string &bytes, unsigned expected) {
    auto id = manifest(d, request(bytes), bytes);
    auto page = d.planPage({id, 0});
    unsigned sent = 0;
    if (auto *ranges = std::get_if<protocol::PlanPage>(&page)) {
        for (auto r : ranges->missingRanges)
            for (auto idx = r.begin; idx < r.end; ++idx)
                ++sent;
        d.planPage({id, 1});
    }
    require(sent == expected);
    auto state = d.query({id});
    for (std::size_t idx = 0; idx < (bytes.size() + 3) / 4; ++idx) {
        auto c = state.missingBitmap[idx / 4];
        unsigned v = c <= '9' ? c - '0' : c - 'a' + 10; // bitmap hex byte high nibble first
        auto hi = state.missingBitmap[(idx / 8) * 2];
        auto lo = state.missingBitmap[(idx / 8) * 2 + 1];
        auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        v = (digit(hi) << 4) | digit(lo);
        if (v & (1U << (idx % 8))) {
            auto data = bytes.substr(idx * 4, 4);
            d.write({id, idx * 4, sha256(data), data});
        }
    }
    auto result = d.commit({id});
    require(d.commit({id}).versionId == result.versionId);
    return result;
}
int main() {
    char temp[] = "/tmp/chunkharbor-delta-XXXXXX";
    require(mkdtemp(temp) != nullptr);
    protocol::VersionId first{}, second{};
    protocol::SessionId pending{};
    protocol::SessionId oldSession{};
    {
        UploadStorage legacy(temp);
        auto meta = request("old!").metadata;
        meta.path = "history";
        auto old = legacy.begin(meta);
        oldSession = old.plan.sessionId;
        legacy.write(old.session, {oldSession, 0, sha256("old!"), "old!"});
        legacy.commit(old.session, {oldSession});
        DeltaStorage delta(legacy);
        auto newer = request("new!");
        newer.metadata.path = "history";
        auto id = delta.begin(newer).sessionId;
        delta.appendManifest({id, 0, 0, {sha256("new!")}});
        delta.finishManifest({id, 1, 1});
        delta.planPage({id, 0});
        delta.planPage({id, 1});
        delta.write({id, 0, sha256("new!"), "new!"});
        delta.commit({id});
        require(legacy.query({oldSession}).result.sha256 == toHex(sha256("old!")));
    }
    {
        UploadStorage legacy(temp);
        require(legacy.query({oldSession}).result.sha256 == toHex(sha256("old!")));
        DeltaStorage delta(legacy);
        first = upload(delta, "aaaabbbb", 2).versionId;
        require(upload(delta, "aaaabbbb", 0).versionId == first);
        second = upload(delta, "aaaacccc", 1).versionId;
        require(second != first);
        require(upload(delta, "aaaabbbb", 1).versionId != first);
        auto preview =
            manifest(delta, request("aaaaxxxx", protocol::DeltaMode::Preview), "aaaaxxxx");
        auto page = std::get<protocol::PlanPage>(delta.planPage({preview, 0}));
        require(page.missingRanges.size() == 1);
        auto done = std::get<protocol::PlanDone>(delta.planPage({preview, 1}));
        require(done.missingBlocks == 1 && done.reusedBlocks == 1);
        require(delta.query({preview}).phase == protocol::DeltaPhase::PlanReady);
        rejected([&] { delta.commit({preview}); });
        for (int n = 0; n < 12; ++n) {
            auto view =
                manifest(delta, request("aaaaxxxx", protocol::DeltaMode::Preview), "aaaaxxxx");
            delta.planPage({view, 0});
            require(std::get<protocol::PlanDone>(delta.planPage({view, 1})).missingBlocks == 1);
        }
        auto collision = request("coll");
        collision.metadata.path = "collision";
        auto cid = manifest(delta, collision, "coll");
        std::string collisionName = ".publish-";
        for (auto byte : cid) {
            collisionName += "0123456789abcdef"[byte >> 4];
            collisionName += "0123456789abcdef"[byte & 15];
        }
        auto protectedFile = request("safe");
        protectedFile.metadata.path = collisionName;
        auto protectedId = manifest(delta, protectedFile, "safe");
        delta.planPage({protectedId, 0});
        delta.planPage({protectedId, 1});
        delta.write({protectedId, 0, sha256("safe"), "safe"});
        delta.commit({protectedId});
        delta.planPage({cid, 0});
        delta.planPage({cid, 1});
        delta.write({cid, 0, sha256("coll"), "coll"});
        delta.commit({cid});
        require(toHex(sha256File(std::filesystem::path(temp) / "files" / collisionName)) ==
                toHex(sha256("safe")));
        pending = manifest(delta, request("zzzzbbbb"), "zzzzbbbb");
        delta.planPage({pending, 0});
        delta.planPage({pending, 1});
        rejected([&] { delta.commit({pending}); });
    }
    std::filesystem::create_directory(std::filesystem::path(temp) /
                                      ".versions/eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
    ::chmod((std::filesystem::path(temp) / ".versions/eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee").c_str(),
            0700);
    {
        UploadStorage legacy(temp);
        DeltaStorage delta(legacy);
        require(std::filesystem::exists(std::filesystem::path(temp) /
                                        ".quarantine/version-eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"));
        require(delta.query({pending}).phase == protocol::DeltaPhase::Transferring);
        delta.write({pending, 0, sha256("zzzz"), "zzzz"});
        delta.commit({pending});
        require(toHex(sha256File(std::filesystem::path(temp) / "files/test/file")) ==
                toHex(sha256("zzzzbbbb")));
    }
    int channel[2];
    require(::pipe(channel) == 0);
    auto child = ::fork();
    require(child >= 0);
    if (child == 0) {
        ::close(channel[0]);
        UploadStorage legacy(temp);
        DeltaStorage delta(legacy);
        auto id = manifest(delta, request("xxxxbbbb"), "xxxxbbbb");
        delta.planPage({id, 0});
        delta.planPage({id, 1});
        delta.write({id, 0, sha256("xxxx"), "xxxx"});
        require(::write(channel[1], id.data(), id.size()) == static_cast<ssize_t>(id.size()));
        killAfterPublish = true;
        delta.commit({id});
        ::_exit(10);
    }
    ::close(channel[1]);
    protocol::SessionId interrupted{};
    require(::read(channel[0], interrupted.data(), interrupted.size()) ==
            static_cast<ssize_t>(interrupted.size()));
    ::close(channel[0]);
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFSIGNALED(status) &&
            WTERMSIG(status) == SIGKILL);
    {
        UploadStorage legacy(temp);
        DeltaStorage delta(legacy);
        require(delta.query({interrupted}).phase == protocol::DeltaPhase::Committed);
        require(delta.commit({interrupted}).sha256 == toHex(sha256("xxxxbbbb")));
        require(toHex(sha256File(std::filesystem::path(temp) / "files/test/file")) ==
                toHex(sha256("xxxxbbbb")));
    }
    std::filesystem::remove_all(temp);
}
