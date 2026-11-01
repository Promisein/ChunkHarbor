#include "DeltaClient.h"
#include "chunkharbor/client_core/BackupBatch.h"
#include "chunkharbor/client_core/BackupPreview.h"
#include "chunkharbor/client_core/DirectoryScanner.h"
#include <chrono>
#include <csignal>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
namespace {
volatile std::sig_atomic_t cancelled = 0;
void interrupt(int) { cancelled = 1; }
std::string hex(const chunkharbor::protocol::SessionId &id) {
    std::ostringstream o;
    for (auto b : id)
        o << std::hex << std::setw(2) << std::setfill('0') << unsigned(b);
    return o.str();
}
chunkharbor::protocol::SessionId parseId(const std::string &text) {
    if (text.size() != 32)
        throw std::runtime_error("invalid saved ID");
    chunkharbor::protocol::SessionId id{};
    std::string digits = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) {
        auto n = digits.find(text[i]);
        if (n == std::string::npos)
            throw std::runtime_error("invalid saved ID");
        id[i / 2] = static_cast<std::uint8_t>((id[i / 2] << 4) | n);
    }
    return id;
}
// Deterministic compact filename; the saved full identity detects key collisions.
std::string stateKey(std::string_view logical) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char byte : logical) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str() + ".state";
}
std::string readSmall(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot read state");
    std::string bytes(16385, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    if (bytes.size() > 16384 || input.bad())
        throw std::runtime_error("state exceeds limit or read failed");
    return bytes;
}
} // namespace
int runDeltaCommand(int argc, char **argv) {
    using namespace chunkharbor::client;
    using namespace chunkharbor::protocol;
    namespace fs = std::filesystem;
    cancelled = 0;
    std::signal(SIGINT, interrupt);
    std::map<std::string, std::string> args;
    std::set<std::string> exclusions;
    for (int i = 2; i < argc; i += 2) {
        if (i + 1 >= argc)
            throw std::runtime_error("missing option value");
        std::string key = argv[i];
        if (key == "--exclude")
            exclusions.insert(argv[i + 1]);
        else if (!args.emplace(key, argv[i + 1]).second)
            throw std::runtime_error("duplicate option");
    }
    for (const auto &p : args)
        if (p.first != "--file" && p.first != "--directory" && p.first != "--path" &&
            p.first != "--prefix" && p.first != "--state" && p.first != "--state-dir" &&
            p.first != "--host" && p.first != "--port" && p.first != "--chunk-size" &&
            p.first != "--base-version" && p.first != "--source-sha256")
            throw std::runtime_error("unknown delta option");
    auto get = [&](const std::string &key, std::string fallback = "") {
        auto it = args.find(key);
        return it == args.end() ? fallback : it->second;
    };
    auto number = [&](const std::string &key, unsigned fallback, unsigned maximum) {
        auto text = get(key);
        if (text.empty())
            return fallback;
        std::size_t used = 0;
        auto n = std::stoul(text, &used);
        if (used != text.size() || !n || n > maximum)
            throw std::runtime_error("invalid numeric option");
        return static_cast<unsigned>(n);
    };
    auto host = get("--host", "127.0.0.1");
    if (host != "127.0.0.1")
        throw std::runtime_error("trusted loopback/SSH endpoint required");
    auto port = number("--port", 8787, 65535),
         chunk = number("--chunk-size", 262144, static_cast<unsigned>(MaxChunkDataSize));
    bool preview = std::string(argv[1]) == "preview";
    BackupBatch batch;
    auto &summary = batch.summary;
    std::string batchToken;
    auto one = [&](const fs::path &file, const std::string &logical, const fs::path &state) {
        auto platform =
            openDeltaPlatform(file, host, static_cast<std::uint16_t>(port), chunk, logical);
        auto &m = platform.source.metadata;
        // Text format stores every integer as decimal, preserving all 64 bits.
        auto identity = [&] {
            std::ostringstream o;
            o << "CHDELTA1\n"
              << std::quoted(host) << ' ' << port << ' '
              << std::quoted(fs::absolute(file).u8string()) << ' ' << std::quoted(m.path) << ' '
              << m.size << ' ' << m.mtimeNs << ' ' << m.chunkSize << ' ' << m.sha256 << ' '
              << preview << '\n';
            return o.str();
        };
        auto prefix = identity();
        auto done = state;
        done += ".done";
        if (!batchToken.empty() && fs::exists(done)) {
            std::ifstream in(done, std::ios::binary);
            std::string text(16385, '\0');
            in.read(text.data(), static_cast<std::streamsize>(text.size()));
            text.resize(static_cast<std::size_t>(in.gcount()));
            if (text.compare(0, batchToken.size() + 1, batchToken + "\n") == 0) {
                if (text != batchToken + "\n" + prefix)
                    throw std::runtime_error(
                        "completed batch source changed; use a new state directory");
                ++summary.succeeded;
                std::cout << "RESUME SKIP completed path=" << logical << '\n';
                return;
            }
        }
        DeltaOptions opt;
        opt.mode = preview ? DeltaMode::Preview : DeltaMode::Upload;
        if (!get("--source-sha256").empty() && get("--source-sha256") != m.sha256)
            throw std::runtime_error("preview source identity changed");
        if (!get("--base-version").empty()) {
            if (get("--base-version") == "absent")
                opt.base.kind = BaseSelector::Kind::Absent;
            else {
                opt.base.kind = BaseSelector::Kind::Exact;
                opt.base.versionId = parseId(get("--base-version"));
            }
        }
        opt.cancelled = [] { return cancelled != 0; };
        if (fs::exists(state)) {
            std::ifstream in(state, std::ios::binary);
            std::string saved(16385, '\0');
            in.read(saved.data(), static_cast<std::streamsize>(saved.size()));
            saved.resize(static_cast<std::size_t>(in.gcount()));
            if (saved.size() > 16384 || saved.compare(0, prefix.size(), prefix) != 0)
                throw std::runtime_error("saved state source/endpoint/mode mismatch");
            std::istringstream tail(saved.substr(prefix.size()));
            std::string session, base;
            if (!(tail >> session >> base))
                throw std::runtime_error("invalid state");
            opt.resume = DeltaCheckpoint{parseId(session),
                                         base == "-" ? std::optional<VersionId>{}
                                                     : std::optional<VersionId>{parseId(base)}};
        }
        opt.save = [&](const DeltaCheckpoint &cp) {
            atomicDeltaState(state, prefix + hex(cp.session) + " " +
                                        (cp.base ? hex(*cp.base) : "-") + "\n");
        };
        auto result = runDelta(platform.source, platform.exchange, opt);
        if (!batchToken.empty())
            atomicDeltaState(done, batchToken + "\n" + prefix);
        fs::remove(state);
        ++summary.succeeded;
        summary.plannedBytes += result.plan.missingBytes;
        summary.acknowledgedBytes += result.acknowledgedBytes;
        const char *kind =
            preview ? (previewChange(result.plan) == ChangeKind::Added      ? "Added"
                       : previewChange(result.plan) == ChangeKind::Modified ? "Modified"
                                                                            : "Unchanged")
                    : "BACKUP OK";
        std::cout << kind << " path=" << logical << " base_version="
                  << (result.plan.baseVersion ? hex(*result.plan.baseVersion) : "absent")
                  << " missing_blocks=" << result.plan.missingBlocks
                  << " missing_bytes=" << result.plan.missingBytes
                  << " blocks_sent_this_run=" << result.sentBlocks << " sha256=" << m.sha256
                  << '\n';
    };
    if (get("--directory").empty()) {
        if (get("--file").empty() || get("--path").empty() || get("--state").empty())
            throw std::runtime_error("backup/preview requires --file --path --state");
        one(fs::u8path(get("--file")), get("--path"), fs::u8path(get("--state")));
        return 0;
    }
    if (!get("--file").empty() || get("--state-dir").empty())
        throw std::runtime_error("directory requires --state-dir and no --file");
    auto stateDir = fs::absolute(fs::u8path(get("--state-dir")));
    fs::create_directories(stateDir);
    auto marker = stateDir / ".batch";
    std::ostringstream configuration;
    configuration << std::quoted(fs::absolute(fs::u8path(get("--directory"))).u8string()) << ' '
                  << host << ' ' << port << ' ' << chunk << ' ' << preview << ' '
                  << std::quoted(get("--prefix"));
    for (const auto &e : exclusions)
        configuration << ' ' << std::quoted(e);
    auto config = configuration.str();
    if (fs::exists(marker)) {
        std::istringstream in(readSmall(marker));
        std::string oldConfig;
        std::getline(in, batchToken);
        std::getline(in, oldConfig);
        if (batchToken.empty() || oldConfig != config)
            throw std::runtime_error("batch state configuration mismatch");
    } else {
        batchToken =
            std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
        atomicDeltaState(marker, batchToken + "\n" + config + "\n");
    }
    if (stateDir.lexically_normal() ==
        fs::absolute(fs::u8path(get("--directory"))).lexically_normal())
        throw std::runtime_error("state directory must differ from source root");
    DirectoryScanner scanner(fs::u8path(get("--directory")), exclusions, stateDir);
    // Pending records remain authoritative when a source disappears between runs.
    std::size_t recordCount = 0;
    for (const auto &saved : fs::directory_iterator(stateDir)) {
        if (++recordCount > 20001)
            throw std::runtime_error("batch state record limit exceeded");
        if (!saved.is_regular_file() || saved.is_symlink())
            continue;
        auto name = saved.path().filename().string();
        bool done = name.size() >= 5 && name.substr(name.size() - 5) == ".done";
        if (!done && saved.path().extension() != ".state")
            continue;
        auto text = readSmall(saved.path());
        std::istringstream lines(text);
        std::string line;
        if (done) {
            std::getline(lines, line);
            if (line != batchToken)
                continue;
        }
        std::getline(lines, line);
        if (line != "CHDELTA1")
            throw std::runtime_error("invalid batch source record");
        std::getline(lines, line);
        std::istringstream fields(line);
        std::string endpoint, local;
        unsigned savedPort = 0;
        if (!(fields >> std::quoted(endpoint) >> savedPort >> std::quoted(local)))
            throw std::runtime_error("invalid batch identity");
        if (!fs::exists(fs::u8path(local))) {
            ++summary.failed;
            std::cerr << "Error missing batch source=" << local << '\n';
        }
    }
    // Stable relative path state keys avoid scan-order dependence on retries.
    while (!cancelled) {
        std::optional<ScanEntry> entry;
        try {
            entry = scanner.next();
        } catch (const std::exception &e) {
            ++summary.failed;
            std::cerr << "Error scanning: " << e.what() << '\n';
            break;
        }
        if (!entry)
            break;
        if (!entry->error.empty()) {
            ++summary.failed;
            std::cerr << "Error path=" << entry->logicalPath << " " << entry->error << '\n';
            continue;
        }
        if (entry->excluded) {
            ++summary.skipped;
            std::cout << "Excluded path=" << entry->logicalPath << '\n';
            continue;
        }
        auto logical = get("--prefix");
        if (!logical.empty())
            logical += '/';
        logical += entry->logicalPath;
        auto state = stateDir / stateKey(entry->logicalPath);
        fs::create_directories(state.parent_path());
        try {
            one(entry->localPath, logical, state);
        } catch (const std::exception &e) {
            ++summary.failed;
            std::cerr << "Error path=" << logical << " " << e.what() << '\n';
        }
    }
    if (cancelled) {
        batch.cancel();
        ++summary.cancelled;
    }
    std::cout << "SUMMARY success=" << summary.succeeded << " failed=" << summary.failed
              << " skipped=" << summary.skipped << " cancelled=" << summary.cancelled
              << " planned_bytes=" << summary.plannedBytes
              << " acknowledged_bytes=" << summary.acknowledgedBytes << '\n';
    if (!summary.failed && !summary.cancelled)
        fs::remove(marker);
    return summary.failed || summary.cancelled ? 2 : 0;
}
