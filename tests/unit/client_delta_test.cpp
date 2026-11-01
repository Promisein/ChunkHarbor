#include "chunkharbor/client_core/DeltaExchange.h"
#include "chunkharbor/protocol/ManifestPage.h"
#include <iostream>
#include <stdexcept>
using namespace chunkharbor::client;
using namespace chunkharbor::protocol;
void check(bool b) {
    if (!b)
        throw std::runtime_error("client delta assertion failed");
}
template <class F> void rejects(F f) {
    bool failed = false;
    try {
        f();
    } catch (const std::exception &) {
        failed = true;
    }
    check(failed);
}
int main() {
    try {
        SourceSnapshot source;
        source.metadata = {"file", 513, 0, 1, std::string(64, '0')};
        source.read = [](auto, auto n) { return std::string(n, 'a'); };
        source.hash = [](auto) { return Digest{}; };
        source.verify = [] {};
        ManifestReader reader(source, {});
        auto first = reader.next(), last = reader.next();
        check(first->hashes.size() == 512 && last->hashes.size() == 1 && !reader.next());
        source.metadata.size = 65536;
        ManifestReader maximum(source, {});
        unsigned pages = 0;
        while (auto page = maximum.next()) {
            check(page->hashes.size() <= 512);
            ++pages;
        }
        check(pages == 128);
        source.metadata.size = 65537;
        ManifestReader excessive(source, {});
        rejects([&] { excessive.next(); });
        source.metadata.size = 0;
        ManifestReader empty(source, {});
        check(!empty.next());
        source.metadata.size = 1;
        source.read = [](auto, auto) { return std::string(); };
        ManifestReader shortRead(source, {});
        rejects([&] { shortRead.next(); });
        source.read = [](auto, auto n) { return std::string(n, 'a'); };
        DeltaOptions options;
        options.save = [](const auto &) {};
        rejects([&] {
            runDelta(
                source,
                [](const Frame &q) {
                    return Frame{25, q.requestId + 1, encodeDeltaReady({{}, std::nullopt, 1})};
                },
                options);
        });
        unsigned calls = 0;
        rejects([&] {
            runDelta(
                source,
                [&](const Frame &q) {
                    ++calls;
                    if (q.type == 24)
                        return Frame{25, q.requestId, encodeDeltaReady({{}, std::nullopt, 1})};
                    if (q.type == 18)
                        return Frame{26, q.requestId,
                                     encodeManifestAck({{}, 1, DeltaPhase::Collecting})};
                    if (q.type == 19)
                        return Frame{26, q.requestId,
                                     encodeManifestAck({{}, 1, DeltaPhase::PlanReady})};
                    return Frame{21, q.requestId,
                                 encodePlanDone({{}, std::nullopt, 1, 1, 0, 0, 1, 1})};
                },
                options);
        });
        check(calls == 4);
        options.save = [](const auto &) { throw std::runtime_error("disk full"); };
        calls = 0;
        rejects([&] {
            runDelta(
                source,
                [&](const Frame &q) {
                    ++calls;
                    return Frame{25, q.requestId, encodeDeltaReady({{}, std::nullopt, 1})};
                },
                options);
        });
        check(calls == 1);
        options.cancelled = [] { return true; };
        calls = 0;
        rejects([&] {
            runDelta(
                source,
                [&](const Frame &q) {
                    ++calls;
                    return q;
                },
                options);
        });
        check(calls == 0);
        options.cancelled = [] { return false; };
        options.save = [](const auto &) {};
        options.mode = DeltaMode::Preview;
        source.metadata.size = 0;
        unsigned verifications = 0;
        source.verify = [&] {
            if (++verifications == 2)
                throw std::runtime_error("source changed during preview");
        };
        rejects([&] {
            runDelta(
                source,
                [](const Frame &q) {
                    if (q.type == 24)
                        return Frame{25, q.requestId, encodeDeltaReady({{}, std::nullopt, 0})};
                    if (q.type == 19)
                        return Frame{26, q.requestId,
                                     encodeManifestAck({{}, 0, DeltaPhase::PlanReady})};
                    return Frame{21, q.requestId,
                                 encodePlanDone({{}, std::nullopt, 0, 0, 0, 0, 0, 0, "Added"})};
                },
                options);
        });
        check(verifications == 2);
        std::cout << "client delta validation passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
