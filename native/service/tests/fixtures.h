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
inline std::string fakeContractDef() {
    return R"(
#include "contracts/B.h"
struct m256i { unsigned long long u64[4]; };
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
