// Test fixtures: a tiny fake core as a git repository (the "remote": a plain path is a valid repository URL), tiny
// state files in temporary directories, an event collector and helpers to call the service through a dispatcher.
#pragma once

#include "git_fixture.h"
#include "qstate/rpc/dispatcher.h"
#include "qstate/rpc/event_bus.h"
#include "qstate/service/service.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace qstate::service::testing {

namespace fs = std::filesystem;
using nlohmann::json;

class TempDir {
public:
    TempDir() {
        std::string pattern = (fs::temp_directory_path() / "qstate-service-test-XXXXXX").string();
        std::vector<char> buf(pattern.begin(), pattern.end());
        buf.push_back('\0');
        const char* made = ::mkdtemp(buf.data());
        REQUIRE(made != nullptr);
        path_ = made;
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const fs::path& path() const { return path_; }
    std::string str() const { return path_.string(); }
    fs::path operator/(const std::string& name) const { return path_ / name; }

private:
    fs::path path_;
};

inline void writeFile(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

inline void writeBytes(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

inline void appendBytes(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// Layout of the fake core:
//   contract 0: Contract0State { long long reserves[2]; }                   16 bytes
//   contract 1: AA, A::StateData { Array<Row, 8> rows; u64 counter; }       328 bytes (Row = { id owner; u64 amount; } 40)
//   contract 2: BB, B::StateData { u64 x; }                                 8 bytes (in src/contracts/B.h)
//   ledgers: EntityRecord / AssetRecord as in the core, SPECTRUM_CAPACITY = ASSETS_CAPACITY = 64 slots
//            (spectrum.EEE = 4096 bytes, universe.EEE = 3072 bytes)
constexpr unsigned kFakeLedgerCapacity = 64;

inline std::string fakeContractDef() {
    return R"(
#include "contracts/B.h"
#define SPECTRUM_DEPTH 6
#define SPECTRUM_CAPACITY (1ULL << SPECTRUM_DEPTH)
#define ASSETS_CAPACITY 0x40ULL
struct m256i { unsigned long long u64[4]; };
struct EntityRecord {
    m256i publicKey;
    long long incomingAmount, outgoingAmount;
    unsigned int numberOfIncomingTransfers, numberOfOutgoingTransfers;
    unsigned int latestIncomingTransferTick, latestOutgoingTransferTick;
};
struct AssetRecord {
    union {
        struct { m256i publicKey; unsigned char type; char name[7]; char numberOfDecimalPlaces; char unitOfMeasurement[7]; } issuance;
        struct { m256i publicKey; unsigned char type; char padding[1]; unsigned short managingContractIndex; unsigned int issuanceIndex; long long numberOfShares; } ownership;
        struct { m256i publicKey; unsigned char type; char padding[1]; unsigned short managingContractIndex; unsigned int ownershipIndex; long long numberOfShares; } possession;
    } varStruct;
};
namespace QPI {
template <typename T, unsigned long long L> struct Array { T _values[L]; };
using id = ::m256i;
}
struct A { struct Row { QPI::id owner; unsigned long long amount; };
           struct StateData { QPI::Array<Row, 8> rows; unsigned long long counter; }; };
struct Contract0State { long long reserves[2]; };
constexpr struct ContractDescription {
    char assetName[8];
    unsigned short constructionEpoch, destructionEpoch;
    unsigned long long stateSize;
} contractDescriptions[] = {
    {"", 0, 0, sizeof(Contract0State)},
    {"AA", 5, 10000, sizeof(A::StateData)},
    {"BB", 6, 10000, sizeof(B::StateData)},
};
)";
}

inline std::string fakeHeaderB(bool extraField = false) {
    return std::string("struct B { struct StateData { unsigned long long x;") + (extraField ? " unsigned long long y;" : "") + " }; };\n";
}

inline std::string fakePublicSettings(int epoch) {
    return "#define VERSION_A 1\n#define VERSION_B 2\n#define VERSION_C 3\n#define EPOCH " + std::to_string(epoch) + "\n";
}

inline void writeFakeCore(const fs::path& root, int epoch = 5, bool extraField = false) {
    writeFile(root / "src/contract_core/contract_def.h", fakeContractDef());
    writeFile(root / "src/contracts/B.h", fakeHeaderB(extraField));
    writeFile(root / "src/public_settings.h", fakePublicSettings(epoch));
}

// The fake core repository:
//   tag v5.0.0 (epoch 5)  <-  tag v5.0.1 (epoch 5, newer)  <-  tag v9.0.0 = main (epoch 9, B has a second field)
//   branch dev: one commit on top of v5.0.1 (epoch 7)
// `repo.sha("v5.0.1")` etc. are the commits of these names.
class FakeCoreRepo {
public:
    explicit FakeCoreRepo(const fs::path& dir) : git(dir) {
        writeFakeCore(dir, 5);
        shaV500 = git.commit("core epoch 5");
        git.tag("v5.0.0");
        writeFile(dir / "src/public_settings.h", fakePublicSettings(5) + "// patch\n");
        shaV501 = git.commit("core epoch 5, patch");
        git.tag("v5.0.1", /*annotated=*/true);
        git.checkoutNew("dev");
        writeFakeCore(dir, 7);
        shaDev = git.commit("work on epoch 7");
        git.checkout("main");
        writeFakeCore(dir, 9, /*extraField=*/true);
        shaV900 = git.commit("core epoch 9");
        git.tag("v9.0.0");
    }
    std::string url() const { return git.url(); }

    qstate::testing::GitFixtureRepo git;
    std::string shaV500, shaV501, shaV900, shaDev;
};

// One fake core for the whole process (immutable: tests that change the upstream make their own FakeCoreRepo).
inline const FakeCoreRepo& sharedCore() {
    static const TempDir* dir = new TempDir();  // lives until the process ends
    static const FakeCoreRepo repo(dir->path() / "core");
    return repo;
}

inline std::vector<std::uint8_t> patternBytes(std::size_t size, std::uint8_t seed) {
    std::vector<std::uint8_t> v(size);
    for (std::size_t i = 0; i < size; ++i) v[i] = static_cast<std::uint8_t>((i * 7 + seed) & 0xFF);
    return v;
}

// State files of the fake core for epoch `ext`: contract0000 (16), contract0001 (328), contract0002 (8).
inline void writeFakeState(const fs::path& dir, unsigned ext = 5) {
    char name[40];
    auto file = [&](unsigned index) {
        std::snprintf(name, sizeof name, "contract%04u.%03u", index, ext);
        return dir / name;
    };
    writeBytes(file(0), patternBytes(16, 1));
    // contract 1: rows[0] = { owner = (1,2,3,4), amount = 42 }, rest zero, counter = 7
    std::vector<std::uint8_t> a(328, 0);
    auto put64 = [&](std::size_t off, std::uint64_t v) {
        for (int i = 0; i < 8; ++i) a[off + i] = static_cast<std::uint8_t>(v >> (8 * i));
    };
    put64(0, 1);
    put64(8, 2);
    put64(16, 3);
    put64(24, 4);
    put64(32, 42);
    put64(320, 7);
    writeBytes(file(1), a);
    writeBytes(file(2), patternBytes(8, 9));
}

// 32-byte key from four little-endian words.
inline std::vector<std::uint8_t> keyBytes(std::uint64_t w0, std::uint64_t w1 = 0, std::uint64_t w2 = 0, std::uint64_t w3 = 0) {
    std::vector<std::uint8_t> v(32);
    const std::uint64_t w[4] = {w0, w1, w2, w3};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j) v[static_cast<std::size_t>(8 * i + j)] = static_cast<std::uint8_t>((w[i] >> (8 * j)) & 0xFF);
    return v;
}

inline void putLe(std::vector<std::uint8_t>& b, std::size_t off, std::uint64_t v, int n) {
    for (int i = 0; i < n; ++i) b[off + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
}

// Ledger files of the fake core (64 slots, hashed by key.u32[0] & 63 with linear probing):
//   spectrum: contract 1 at slot 1 (balance 900), key (5, 11) at slot 5 (balance 500), key (69, 1) collides -> slot 6 (50)
//   universe: NOST issued by key (3, 5) at slot 3, owned by key (10, 6) at slot 10 (2500 shares, contract 1),
//             possessed by key (74, 7) at slot 11
inline std::vector<std::uint8_t> fakeSpectrum() {
    std::vector<std::uint8_t> b(kFakeLedgerCapacity * 64, 0);
    auto add = [&](std::uint32_t slot, const std::vector<std::uint8_t>& key, std::int64_t in, std::int64_t out) {
        std::copy(key.begin(), key.end(), b.begin() + slot * 64);
        putLe(b, slot * 64 + 32, static_cast<std::uint64_t>(in), 8);
        putLe(b, slot * 64 + 40, static_cast<std::uint64_t>(out), 8);
        putLe(b, slot * 64 + 48, 1, 4);
    };
    add(1, keyBytes(1), 1000, 100);
    add(5, keyBytes(5, 11), 500, 0);
    add(6, keyBytes(69, 1), 70, 20);
    return b;
}

inline std::vector<std::uint8_t> fakeUniverse() {
    std::vector<std::uint8_t> b(kFakeLedgerCapacity * 48, 0);
    auto rec = [&](std::uint32_t slot, const std::vector<std::uint8_t>& key, std::uint8_t type) {
        std::copy(key.begin(), key.end(), b.begin() + slot * 48);
        b[slot * 48 + 32] = type;
    };
    rec(3, keyBytes(3, 5), 1);
    const std::string name = "NOST";
    std::copy(name.begin(), name.end(), b.begin() + 3 * 48 + 33);
    rec(10, keyBytes(10, 6), 2);
    putLe(b, 10 * 48 + 34, 1, 2);
    putLe(b, 10 * 48 + 36, 3, 4);
    putLe(b, 10 * 48 + 40, 2500, 8);
    rec(11, keyBytes(74, 7), 3);
    putLe(b, 11 * 48 + 34, 1, 2);
    putLe(b, 11 * 48 + 36, 10, 4);
    putLe(b, 11 * 48 + 40, 2500, 8);
    return b;
}

inline void writeFakeLedgers(const fs::path& dir, unsigned ext = 5) {
    char name[40];
    std::snprintf(name, sizeof name, "spectrum.%03u", ext);
    writeBytes(dir / name, fakeSpectrum());
    std::snprintf(name, sizeof name, "universe.%03u", ext);
    writeBytes(dir / name, fakeUniverse());
}

inline json ledgerOf(const json& workspace, const std::string& kind) {
    for (const json& l : workspace["ledgers"]) {
        if (l["kind"] == kind) return l;
    }
    return json();
}

// Collects bus events.
class EventCollector {
public:
    struct Event {
        std::string name;
        json payload;
    };

    explicit EventCollector(rpc::EventBus& bus) {
        sub_ = bus.subscribeScoped([this](const std::string& name, const json& payload) {
            {
                std::lock_guard lock(mutex_);
                events_.push_back({name, payload});
            }
            cv_.notify_all();
        });
    }

    // Waits for an event with this name satisfying `pred` that arrived after `fromIndex`; returns it or null json.
    json waitFor(const std::string& name, std::chrono::milliseconds timeout = std::chrono::milliseconds(8000),
                 const std::function<bool(const json&)>& pred = {}, std::size_t fromIndex = 0) {
        std::unique_lock lock(mutex_);
        json found;
        cv_.wait_for(lock, timeout, [&] {
            for (std::size_t i = fromIndex; i < events_.size(); ++i) {
                if (events_[i].name == name && (!pred || pred(events_[i].payload))) {
                    found = events_[i].payload;
                    return true;
                }
            }
            return false;
        });
        return found;
    }

    std::size_t count(const std::string& name) {
        std::lock_guard lock(mutex_);
        std::size_t n = 0;
        for (const Event& e : events_) n += e.name == name ? 1 : 0;
        return n;
    }
    std::size_t size() {
        std::lock_guard lock(mutex_);
        return events_.size();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Event> events_;
    rpc::EventBus::Subscription sub_;
};

// A service registered on its own dispatcher + bus with fast watcher timings.
struct Harness {
    rpc::Dispatcher dispatcher{4};
    rpc::EventBus bus;
    std::unique_ptr<Service> service;
    TempDir scratch; // settings file and the git mirrors / exports (cache) live here

    explicit Harness(const std::function<void(ServiceConfig&)>& tweak = {}) {
        ServiceConfig config;
        config.settingsPath = (scratch.path() / "settings.json").string();
        config.cacheDir = (scratch.path() / "cache").string();
        config.watcher.pollInterval = std::chrono::milliseconds(20);
        config.watcher.settle = std::chrono::milliseconds(60);
        config.watcher.settleLarge = std::chrono::milliseconds(120);
        if (tweak) tweak(config);
        service = std::make_unique<Service>(config);
        service->registerAll(dispatcher, bus);
    }

    // Returns the whole response ({"result"} or {"error"}).
    json call(const std::string& method, const json& params = json::object()) { return dispatcher.dispatch(method, params); }

    // Returns the result; fails the test with the error message otherwise.
    json ok(const std::string& method, const json& params = json::object()) {
        json r = call(method, params);
        INFO(method << " -> " << r.dump().substr(0, 600));
        REQUIRE(r.contains("result"));
        return r["result"];
    }

    std::string errorCode(const std::string& method, const json& params = json::object()) {
        json r = call(method, params);
        INFO(method << " -> " << r.dump().substr(0, 600));
        REQUIRE(r.contains("error"));
        return r["error"]["code"].get<std::string>();
    }
};

// A workspace request for the shared fake core and a fresh directory with the epoch-5 state files.
struct Fixture {
    TempDir state;
    const FakeCoreRepo& core = sharedCore();
    Fixture() { writeFakeState(state.path()); }
    static json requestFor(const std::string& repoUrl, const std::string& ref, const std::string& statePath) {
        return {{"core", {{"repoUrl", repoUrl}, {"ref", ref}}}, {"statePath", statePath}};
    }
    json request(const std::string& ref = "v5.0.0") const { return requestFor(core.url(), ref, state.str()); }
};

inline bool hasDiagnostic(const json& ws, const std::string& severity, const std::string& text) {
    for (const json& d : ws["diagnostics"]) {
        if (d["severity"] == severity && d["message"].get<std::string>().find(text) != std::string::npos) return true;
    }
    return false;
}

inline json contractOf(const json& workspace, unsigned index) {
    for (const json& c : workspace["contracts"]) {
        if (c["index"] == index) return c;
    }
    return json();
}

} // namespace qstate::service::testing
