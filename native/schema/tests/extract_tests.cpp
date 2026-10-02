#include <doctest/doctest.h>

#include <filesystem>
#include <iostream>

#include "qstate/schema/extract.h"
#include "test_env.h"

using namespace qstate;
namespace fs = std::filesystem;

namespace {

bool hasErrors(const schema::Schema& s) {
    for (const auto& d : s.diags)
        if (d.severity == cpp::Diag::Severity::Error) return true;
    return false;
}

void printErrors(const schema::Schema& s) {
    for (const auto& d : s.diags)
        if (d.severity == cpp::Diag::Severity::Error) std::cerr << "  error: " << d.message << " " << d.file << ":" << d.line << "\n";
}

} // namespace

TEST_CASE("extract: tiny synthetic core") {
    cpp::MapSource src;
    src.add("src/contract_core/contract_def.h", R"(
        namespace QPI { template <typename T, unsigned long long L> struct Array { T _values[L]; }; struct bit { char c; }; }
        struct A { struct StateData { QPI::Array<unsigned long long, 4> a; int x; }; };
        struct Contract0State { long long reserves[2]; };
        constexpr struct ContractDescription {
            char assetName[8];
            unsigned short constructionEpoch, destructionEpoch;
            unsigned long long stateSize;
        } contractDescriptions[] = {
            {"", 0, 0, sizeof(Contract0State)},
            {"AA", 5, 10000, sizeof(A::StateData)},
        };
    )");
    schema::ExtractResult r = schema::extractSchema(src);
    REQUIRE(r.schema);
    CHECK_FALSE(hasErrors(*r.schema));
    REQUIRE(r.schema->contracts.size() == 2);
    const auto& c0 = r.schema->contracts[0];
    CHECK(c0.stateTypeName == "Contract0State");
    CHECK(c0.expectedSize == 16);
    const auto& c1 = r.schema->contracts[1];
    CHECK(c1.name == "AA");
    CHECK(c1.structName == "A");
    CHECK(c1.constructionEpoch == 5);
    CHECK(c1.expectedSize == 40);
    REQUIRE(c1.stateType != schema::kNoType);
    const schema::Type& st = r.schema->type(c1.stateType);
    CHECK(st.size == 40);
    REQUIRE(st.fields.size() == 2);
    CHECK(st.fields[0].name == "a");
    const schema::Type& arr = r.schema->type(st.fields[0].type);
    CHECK(arr.role.kind == schema::RoleKind::Array);
    CHECK(arr.role.capacity == 4);
    CHECK(arr.role.element != schema::kNoType);
}

TEST_CASE("extract: missing root reports an error instead of throwing") {
    cpp::MapSource src;
    schema::ExtractResult r = schema::extractSchema(src);
    REQUIRE(r.schema);
    CHECK(hasErrors(*r.schema));
    CHECK(r.schema->contracts.empty());
}

TEST_CASE("extract: real core (HEAD) lays out every contract") {
    const std::string core = testing::coreRepo();
    if (core.empty()) { MESSAGE("QSTATE_TEST_CORE_REPO not set, skipping"); return; }
    cpp::DiskSource src(core);
    schema::ExtractResult r = schema::extractSchema(src);
    printErrors(*r.schema);
    CHECK_FALSE(hasErrors(*r.schema));
    CHECK(r.schema->contracts.size() >= 31);
    for (const auto& c : r.schema->contracts) {
        INFO("contract " << c.index << " " << c.name << ": " << c.error);
        CHECK(c.stateType != schema::kNoType);
        CHECK(c.expectedSize > 0);
        if (c.stateType != schema::kNoType) CHECK(r.schema->type(c.stateType).size == c.expectedSize);
    }
    std::cerr << "[extract] HEAD: " << r.stats.fileCount << " files, " << r.stats.tokenCount << " tokens, pp "
              << r.stats.preprocessMs << " ms, parse " << r.stats.parseMs << " ms, layout " << r.stats.layoutMs
              << " ms, total " << r.stats.totalMs << " ms, types " << r.schema->types.size() << "\n";
}

// Core snapshot matching the sample state files (v1.303.2 / epoch 229) + the files themselves.
TEST_CASE("extract: epoch-229 snapshot sizes equal the real state file sizes") {
    const std::string core = testing::coreDir229();
    const std::string state = testing::stateDir();
    if (core.empty() || state.empty()) { MESSAGE("QSTATE_TEST_CORE_DIR_229 / QSTATE_TEST_STATE_DIR not set, skipping"); return; }
    cpp::DiskSource src(core);
    schema::ExtractResult r = schema::extractSchema(src);
    printErrors(*r.schema);
    CHECK_FALSE(hasErrors(*r.schema));
    int compared = 0;
    for (const auto& c : r.schema->contracts) {
        char name[32];
        std::snprintf(name, sizeof name, "contract%04u.229", c.index);
        const fs::path p = fs::path(state) / name;
        if (!fs::exists(p)) continue;
        INFO("contract " << c.index << " " << c.name << " file " << p);
        CHECK(fs::file_size(p) == c.expectedSize);
        ++compared;
    }
    CHECK(compared >= 29);
}
