// Real-data validation of the declaration parser / layout engine against the Qubic core headers:
//  * g++ -E outputs (QSTATE_TEST_II_DIR) and the real pipeline (QSTATE_TEST_CORE_DIR through the preprocessor),
//  * contract table sizes vs real state files (QSTATE_TEST_STATE_DIR, epoch 229),
//  * per-field offsets of all state types vs docs/research/data/contract-layouts-{a,b}.json and the g++ oracle files.
// Every test skips (with a message) when its data is missing.
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "program_test_util.h"
#include "qstate/cpp/preprocessor.h"

using namespace qstate::cpp;
using namespace qstate::cpp::testutil;
using nlohmann::json;

namespace {

struct Dataset {
    std::unique_ptr<Program> prog;
    double parseMs = 0;
    std::size_t tokens = 0;
    Diags diags;
};

double nowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A g++ -E file from QSTATE_TEST_II_DIR, parsed once.
Dataset* iiDataset(const std::string& fileName) {
    static std::map<std::string, std::unique_ptr<Dataset>> cache;
    const std::string dir = qstate::testing::envOr("QSTATE_TEST_II_DIR");
    if (dir.empty()) return nullptr;
    auto it = cache.find(fileName);
    if (it != cache.end()) return it->second.get();
    IiUnit unit;
    if (!loadIi(dir + "/" + fileName, unit)) {
        cache[fileName] = nullptr;
        return nullptr;
    }
    auto ds = std::make_unique<Dataset>();
    ds->tokens = unit.tokens.size();
    const double t0 = nowMs();
    ds->prog = Program::parse(std::move(unit.tokens), std::move(unit.files), ds->diags);
    ds->parseMs = nowMs() - t0;
    Dataset* raw = ds.get();
    cache[fileName] = std::move(ds);
    return raw;
}

// The real pipeline: preprocessor + parser on a core checkout.
Dataset* coreDataset(const std::string& coreDir, const std::string& key) {
    static std::map<std::string, std::unique_ptr<Dataset>> cache;
    if (coreDir.empty()) return nullptr;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second.get();
    DiskSource source(coreDir);
    PreprocessOptions opt;
    opt.includeDirs = {"src", ""};
    opt.defines = {{"_MSC_VER", "1944"}, {"_WIN64", "1"}, {"_M_X64", "1"}};
    PreprocessResult pp = Preprocessor(source, opt).run("src/contract_core/contract_def.h");
    auto ds = std::make_unique<Dataset>();
    ds->tokens = pp.tokens.size();
    const double t0 = nowMs();
    ds->prog = Program::parse(std::move(pp.tokens), std::move(pp.files), ds->diags);
    ds->parseMs = nowMs() - t0;
    Dataset* raw = ds.get();
    cache[key] = std::move(ds);
    return raw;
}

std::string v1303CoreDir() { return qstate::testing::envOr("QSTATE_TEST_CORE_V1303_DIR"); }

struct ContractRow {
    std::string assetName;
    std::uint64_t constructionEpoch = 0;
    std::uint64_t stateSize = 0;
};

std::vector<ContractRow> contractRows(Program& prog) {
    std::vector<ContractRow> rows;
    auto var = prog.lookupVariable("contractDescriptions");
    REQUIRE(var.has_value());
    auto init = prog.evalInitializer(*var);
    REQUIRE(init.has_value());
    REQUIRE(init->kind == InitValue::Kind::List);
    for (const InitValue& row : init->items) {
        REQUIRE(row.items.size() == 4);
        REQUIRE(row.names.size() == 4);
        CHECK(row.names[0] == "assetName");
        CHECK(row.names[3] == "stateSize");
        ContractRow r;
        r.assetName = row.items[0].s;
        r.constructionEpoch = static_cast<std::uint64_t>(row.items[1].i);
        REQUIRE_MESSAGE(row.items[3].kind == InitValue::Kind::Int, r.assetName << ": " << row.items[3].error);
        r.stateSize = static_cast<std::uint64_t>(row.items[3].i);
        rows.push_back(r);
    }
    return rows;
}

// ---- ground truth -----------------------------------------------------------------------------------------------

struct ExpField {
    std::string name;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};
struct ExpType {
    std::string name;
    std::string kind;
    std::uint64_t size = 0;
    std::uint64_t align = 0;
    std::vector<ExpField> fields;
};

std::uint64_t uget(const json& j, const char* key, std::uint64_t def = 0) {
    auto it = j.find(key);
    return (it != j.end() && it->is_number_unsigned()) ? it->get<std::uint64_t>() : def;
}

json loadJson(const std::string& name) {
    std::ifstream in(dataFile(name));
    if (!in) return json();
    return json::parse(in, nullptr, false);
}

std::vector<ExpType> expectedTypes(const json& versionNode) {
    std::vector<ExpType> out;
    if (!versionNode.contains("types")) return out;
    for (auto it = versionNode["types"].begin(); it != versionNode["types"].end(); ++it) {
        const json& t = it.value();
        ExpType e;
        e.name = it.key();
        e.kind = t.value("kind", "");
        e.size = uget(t, "size");
        e.align = uget(t, "align");
        if (t.contains("fields"))
            for (const json& f : t["fields"]) e.fields.push_back({f.value("name", ""), uget(f, "offset"), uget(f, "size")});
        out.push_back(std::move(e));
    }
    return out;
}

struct Report {
    int checked = 0;
    int unresolved = 0;
    std::vector<std::string> mismatches;
    std::vector<std::string> unresolvedNames;
    void mismatch(const std::string& s) {
        if (mismatches.size() < 40) mismatches.push_back(s);
        else if (mismatches.size() == 40) mismatches.push_back("...");
    }
};

void compareType(Program& prog, const ExpType& e, Report& rep) {
    TypeId id = prog.lookupType(e.name);
    if (id == kNoType) {
        ++rep.unresolved;
        if (rep.unresolvedNames.size() < 20) rep.unresolvedNames.push_back(e.name);
        return;
    }
    const TypeLayout& l = prog.layoutOf(id);
    ++rep.checked;
    if (!l.complete) {
        rep.mismatch(e.name + ": layout failed: " + l.error);
        return;
    }
    if (l.size != e.size) rep.mismatch(e.name + ": size " + std::to_string(l.size) + " != " + std::to_string(e.size));
    if (e.align && l.align != e.align) rep.mismatch(e.name + ": align " + std::to_string(l.align) + " != " + std::to_string(e.align));
    if (e.kind == "enum" || e.fields.empty()) return;
    std::size_t k = 0;
    for (const ExpField& f : e.fields) {
        // find next field with this name (declarators of anonymous members are not in the expectation lists)
        while (k < l.fields.size() && l.fields[k].name != f.name) ++k;
        if (k >= l.fields.size()) {
            rep.mismatch(e.name + "." + f.name + ": field not found");
            k = 0;
            continue;
        }
        if (l.fields[k].offset != f.offset)
            rep.mismatch(e.name + "." + f.name + ": offset " + std::to_string(l.fields[k].offset) + " != " + std::to_string(f.offset));
        if (l.fields[k].size != f.size)
            rep.mismatch(e.name + "." + f.name + ": size " + std::to_string(l.fields[k].size) + " != " + std::to_string(f.size));
        ++k;
    }
}

std::string joinMismatches(const Report& rep) {
    std::string s;
    for (const std::string& m : rep.mismatches) s += "\n  " + m;
    return s;
}

// Oracle files from g++: "S name|size|align" and "O name|field|offset".
Report compareOracle(Program& prog, const std::string& file) {
    Report rep;
    std::ifstream in(dataFile(file));
    std::string line;
    struct Rec { std::uint64_t size = 0, align = 0; std::vector<std::pair<std::string, std::uint64_t>> offsets; };
    std::vector<std::pair<std::string, Rec>> recs;
    while (std::getline(in, line)) {
        if (line.size() < 3) continue;
        std::vector<std::string> parts;
        std::stringstream ss(line.substr(2));
        std::string part;
        while (std::getline(ss, part, '|')) parts.push_back(part);
        if (line[0] == 'S' && parts.size() == 3) {
            recs.emplace_back(parts[0], Rec{std::stoull(parts[1]), std::stoull(parts[2]), {}});
        } else if (line[0] == 'O' && parts.size() == 3 && !recs.empty()) {
            recs.back().second.offsets.emplace_back(parts[1], std::stoull(parts[2]));
        }
    }
    for (auto& [name, rec] : recs) {
        TypeId id = prog.lookupType(name);
        if (id == kNoType) {
            ++rep.unresolved;
            if (rep.unresolvedNames.size() < 20) rep.unresolvedNames.push_back(name);
            continue;
        }
        const TypeLayout& l = prog.layoutOf(id);
        ++rep.checked;
        if (!l.complete) { rep.mismatch(name + ": layout failed: " + l.error); continue; }
        if (l.size != rec.size) rep.mismatch(name + ": size " + std::to_string(l.size) + " != " + std::to_string(rec.size));
        if (l.align != rec.align) rep.mismatch(name + ": align " + std::to_string(l.align) + " != " + std::to_string(rec.align));
        for (auto& [fname, off] : rec.offsets) {
            const FieldLayout* found = nullptr;
            for (const FieldLayout& f : l.fields)
                if (f.name == fname) { found = &f; break; }
            if (!found) rep.mismatch(name + "." + fname + ": field not found");
            else if (found->offset != off) rep.mismatch(name + "." + fname + ": offset " + std::to_string(found->offset) + " != " + std::to_string(off));
        }
    }
    return rep;
}

// ---- tests ------------------------------------------------------------------------------------------------------

bool noWarnings(const Dataset& ds, std::vector<std::string>* ignored = nullptr) {
    bool ok = true;
    for (const Diag& d : ds.prog->diags()) {
        if (d.severity == Diag::Severity::Note) continue;
        // known: Computors is declared under #pragma pack(1), which the declaration parser does not see
        if (d.message.find("sizeof(Computors)") != std::string::npos) {
            if (ignored) ignored->push_back(d.message);
            continue;
        }
        MESSAGE("diag: " << d.file << ":" << d.line << " " << d.message);
        ok = false;
    }
    return ok;
}

void checkContractTable(Dataset& ds, const std::string& label, const std::string& epochSuffix, bool compareToFiles) {
    Program& prog = *ds.prog;
    std::vector<ContractRow> rows = contractRows(prog);
    CHECK(rows.size() >= 29);
    MESSAGE(label << ": " << rows.size() << " contracts, parse " << ds.parseMs << " ms, " << ds.tokens << " tokens");
    const std::string stateDir = qstate::testing::stateDir();
    int compared = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "contract%04d.%s", static_cast<int>(i), epochSuffix.c_str());
        if (!compareToFiles || stateDir.empty()) continue;
        std::error_code ec;
        auto sz = std::filesystem::file_size(stateDir + "/" + buf, ec);
        if (ec) continue;
        ++compared;
        CHECK_MESSAGE(rows[i].stateSize == sz, label << " " << rows[i].assetName << " (" << i << "): sizeof = " << rows[i].stateSize << ", file " << buf << " = " << sz);
    }
    if (compareToFiles) MESSAGE(label << ": compared " << compared << " rows with real state files");
}

TEST_CASE("program: v1.303.2 contract table equals the real epoch-229 file sizes (g++ -E input)") {
    for (const char* name : {"v1.303.2.node.generic.ii", "v1.303.2.node.msvclike.ii"}) {
        Dataset* ds = iiDataset(name);
        if (!ds) {
            MESSAGE("QSTATE_TEST_II_DIR / " << std::string(name) << " missing, skipping");
            continue;
        }
        SUBCASE(name) {
            checkContractTable(*ds, name, "229", true);
            CHECK(noWarnings(*ds));
        }
    }
}

TEST_CASE("program: expected sizes of the v1.303.2 state types without state files") {
    Dataset* ds = iiDataset("v1.303.2.node.generic.ii");
    if (!ds) { MESSAGE("QSTATE_TEST_II_DIR missing, skipping"); return; }
    std::vector<ContractRow> rows = contractRows(*ds->prog);
    // sizes from docs/research/01 section 6.1 (file sizes of epoch 229)
    const std::map<std::string, std::uint64_t> expected = {
        {"QX", 621806120ull}, {"QTRY", 923559560ull}, {"QUTIL", 402895104ull}, {"GQMPROP", 709184ull},
        {"CCF", 493584ull}, {"NOST", 1030098088ull}, {"QBOND", 357237432ull}, {"QIP", 26906696ull},
        {"QRP", 27040ull}, {"ESCROW", 403505400ull}, {"GGWP", 3669088ull}, {"MLM", 27040ull}, {"SWATCH", 27040ull}};
    int found = 0;
    for (const ContractRow& r : rows) {
        auto it = expected.find(r.assetName);
        if (it == expected.end()) continue;
        ++found;
        CHECK_MESSAGE(r.stateSize == it->second, r.assetName);
    }
    CHECK(found == static_cast<int>(expected.size()));
    CHECK(rows[0].stateSize == 8192);
}

TEST_CASE("program: v1.303.2 layouts equal the ground truth JSON (a + b) and the g++ oracle") {
    Dataset* ds = iiDataset("v1.303.2.node.generic.ii");
    if (!ds) { MESSAGE("QSTATE_TEST_II_DIR missing, skipping"); return; }
    Program& prog = *ds->prog;
    for (const char* file : {"contract-layouts-a.json", "contract-layouts-b.json"}) {
        json j = loadJson(file);
        if (j.is_discarded() || j.is_null()) { MESSAGE(file << " missing, skipping"); continue; }
        const json& v = j["versions"]["v1.303.2"];
        Report rep;
        for (const ExpType& e : expectedTypes(v)) compareType(prog, e, rep);
        MESSAGE(file << " v1.303.2: " << rep.checked << " types compared, " << rep.unresolved << " not resolved");
        for (const std::string& n : rep.unresolvedNames) MESSAGE("  not resolved: " << n);
        CHECK_MESSAGE(rep.mismatches.empty(), file << joinMismatches(rep));
        CHECK(rep.checked > 100);
        // state types per contract
        Report crep;
        auto checkContract = [&](const json& c) {
            std::string stateType = c.contains("stateDataType") && c["stateDataType"].is_string() ? c["stateDataType"].get<std::string>() : c.value("stateType", "");
            std::uint64_t size = c.contains("stateDataSize") ? uget(c, "stateDataSize") : uget(c, "size");
            std::vector<ExpField> fields;
            if (c.contains("fields"))
                for (const json& f : c["fields"]) fields.push_back({f.value("name", ""), uget(f, "offset"), uget(f, "size")});
            ExpType e;
            e.name = stateType;
            e.size = size;
            e.fields = fields;
            compareType(prog, e, crep);
        };
        if (v["contracts"].is_object()) for (auto& c : v["contracts"]) checkContract(c);
        else for (auto& c : v["contracts"]) checkContract(c);
        MESSAGE(file << " state types: " << crep.checked << " compared, " << crep.unresolved << " not in this unit");
        CHECK_MESSAGE(crep.mismatches.empty(), file << joinMismatches(crep));
    }
    Report orc = compareOracle(prog, "contract-layouts-a.oracle.v1.303.2.txt");
    MESSAGE("oracle v1.303.2: " << orc.checked << " types, " << orc.unresolved << " not resolved");
    CHECK_MESSAGE(orc.mismatches.empty(), joinMismatches(orc));
    CHECK(orc.checked > 100);
}

TEST_CASE("program: HEAD layouts equal the ground truth JSON (a + b) and the g++ oracle") {
    for (const char* name : {"HEAD.node.generic.ii", "HEAD.node.msvclike.ii"}) {
        Dataset* ds = iiDataset(name);
        if (!ds) { MESSAGE("QSTATE_TEST_II_DIR / " << std::string(name) << " missing, skipping"); continue; }
        Program& prog = *ds->prog;
        SUBCASE(name) {
            checkContractTable(*ds, name, "233", false);
            CHECK(noWarnings(*ds));
            std::vector<ContractRow> rows = contractRows(prog);
            for (const char* file : {"contract-layouts-a.json", "contract-layouts-b.json"}) {
                json j = loadJson(file);
                if (j.is_discarded() || j.is_null()) continue;
                const json& v = j["versions"]["HEAD"];
                Report rep;
                for (const ExpType& e : expectedTypes(v)) compareType(prog, e, rep);
                MESSAGE(name << " " << file << ": " << rep.checked << " types compared, " << rep.unresolved << " not resolved");
                CHECK_MESSAGE(rep.mismatches.empty(), file << joinMismatches(rep));
                // contract table row vs JSON state size (stateSize of the table entry)
                auto checkRow = [&](const json& c) {
                    if (!c.contains("index") || !c["index"].is_number_unsigned()) return;
                    std::size_t idx = c["index"].get<std::size_t>();
                    std::uint64_t expectedSize = c.contains("stateSize") && c["stateSize"].is_number_unsigned() ? uget(c, "stateSize") : uget(c, "size");
                    if (idx >= rows.size()) return;
                    CHECK_MESSAGE(rows[idx].stateSize == expectedSize, name << " row " << idx << " " << rows[idx].assetName);
                };
                if (v["contracts"].is_object()) for (auto& c : v["contracts"]) checkRow(c);
                else for (auto& c : v["contracts"]) checkRow(c);
            }
            Report orc = compareOracle(prog, "contract-layouts-a.oracle.HEAD.txt");
            MESSAGE(name << " oracle HEAD: " << orc.checked << " types, " << orc.unresolved << " not resolved");
            CHECK_MESSAGE(orc.mismatches.empty(), joinMismatches(orc));
            CHECK(orc.checked > 100);
        }
    }
}

TEST_CASE("program: real pipeline (preprocessor + parser) on the HEAD core checkout") {
    const std::string core = qstate::testing::coreDir();
    if (core.empty()) { MESSAGE("QSTATE_TEST_CORE_DIR not set, skipping"); return; }
    Dataset* ds = coreDataset(core, "head");
    REQUIRE(ds);
    Program& prog = *ds->prog;
    MESSAGE("HEAD: " << ds->tokens << " tokens parsed in " << ds->parseMs << " ms, " << prog.declarationCount() << " declarations");
    CHECK(ds->parseMs < 1000);
    CHECK(noWarnings(*ds));
    // first query of this Program: lays out all state types through the sizeof expressions of the contract table
    const double t0 = nowMs();
    std::vector<ContractRow> rows = contractRows(prog);
    const double ms = nowMs() - t0;
    MESSAGE("contract table with all state layouts: " << ms << " ms");
    CHECK(ms < 1000);
    CHECK(rows.size() >= 31);
    json j = loadJson("contract-layouts-a.json");
    if (!j.is_discarded() && !j.is_null()) {
        Report rep;
        for (const ExpType& e : expectedTypes(j["versions"]["HEAD"])) compareType(prog, e, rep);
        CHECK_MESSAGE(rep.mismatches.empty(), joinMismatches(rep));
        CHECK(rep.checked > 100);
    }
}

TEST_CASE("program: real pipeline on the v1.303.2 checkout matches the state files") {
    const std::string core = v1303CoreDir();
    if (core.empty()) { MESSAGE("QSTATE_TEST_CORE_V1303_DIR not set, skipping"); return; }
    Dataset* ds = coreDataset(core, "v1303");
    REQUIRE(ds);
    CHECK(noWarnings(*ds));
    checkContractTable(*ds, "v1.303.2 (real pipeline)", "229", true);
}

TEST_CASE("program: test-example translation unit (INCLUDE_CONTRACT_TEST_EXAMPLES) matches the ground truth") {
    for (const char* version : {"v1.303.2", "HEAD"}) {
        const std::string fileName = std::string(version) + ".test.generic.ii";
        Dataset* ds = iiDataset(fileName);
        if (!ds) { MESSAGE("QSTATE_TEST_II_DIR / " << fileName << " missing, skipping"); continue; }
        Program& prog = *ds->prog;
        std::vector<ContractRow> rows = contractRows(prog);
        json b = loadJson("contract-layouts-b.json");
        if (b.is_discarded() || b.is_null()) continue;
        const json& v = b["versions"][version];
        Report rep;
        for (const ExpType& e : expectedTypes(v)) compareType(prog, e, rep);
        MESSAGE(fileName << ": " << rep.checked << " types compared, " << rep.unresolved << " not resolved");
        for (const std::string& n : rep.unresolvedNames) MESSAGE("  not resolved: " << n);
        CHECK_MESSAGE(rep.mismatches.empty(), fileName << joinMismatches(rep));
        for (const json& c : v["contracts"]) {
            if (!c.contains("index") || !c["index"].is_number_unsigned()) continue;
            std::size_t idx = c["index"].get<std::size_t>();
            if (idx >= rows.size()) continue;
            CHECK_MESSAGE(rows[idx].stateSize == uget(c, "stateSize"), fileName << " row " << idx << " " << rows[idx].assetName);
        }
    }
}

TEST_CASE("program: contractStateChangeInfos of HEAD is readable as rows (index, change type, epoch)") {
    Dataset* ds = iiDataset("HEAD.node.generic.ii");
    if (!ds) { MESSAGE("QSTATE_TEST_II_DIR missing, skipping"); return; }
    Program& prog = *ds->prog;
    auto var = prog.lookupVariable("contractStateChangeInfos");
    REQUIRE(var.has_value());
    auto init = prog.evalInitializer(*var);
    REQUIRE(init.has_value());
    REQUIRE(init->items.size() == 1);
    const InitValue& row = init->items[0];
    REQUIRE(row.items.size() == 3);
    CHECK(row.names[0] == "contractIndex");
    CHECK(row.names[1] == "changeType");
    {
        std::vector<ContractRow> rows = contractRows(prog);
        REQUIRE(static_cast<std::size_t>(row.items[0].i) < rows.size());
        CHECK(rows[static_cast<std::size_t>(row.items[0].i)].assetName == "NOST");
    }
    CHECK(row.items[1].i == prog.evalConstant("MIGRATE").value().value);
    CHECK(row.items[2].i == 230);
    auto count = prog.lookupVariable("contractCount");
    REQUIRE(count.has_value());
    CHECK(prog.evalConstant("contractCount").value().value == 31);
}

TEST_CASE("program: truncated and mutilated real translation units never crash or hang") {
    const std::string dir = qstate::testing::envOr("QSTATE_TEST_II_DIR");
    if (dir.empty()) { MESSAGE("QSTATE_TEST_II_DIR not set, skipping"); return; }
    IiUnit unit;
    if (!loadIi(dir + "/v1.303.2.node.generic.ii", unit)) { MESSAGE("v1.303.2.node.generic.ii missing, skipping"); return; }
    const std::size_t n = unit.tokens.size();
    std::mt19937 rng(777);
    for (int iter = 0; iter < 24; ++iter) {
        std::vector<Token> t;
        if (iter < 12) {
            t.assign(unit.tokens.begin(), unit.tokens.begin() + static_cast<std::ptrdiff_t>(n * static_cast<std::size_t>(iter + 1) / 13));
            Token end;
            end.kind = TokKind::End;
            t.push_back(end);
        } else {
            t = unit.tokens;
            for (int e = 0; e < 200; ++e) {
                std::size_t i = rng() % (t.size() - 1);
                if (rng() % 2) t.erase(t.begin() + static_cast<std::ptrdiff_t>(i));
                else std::swap(t[i], t[rng() % (t.size() - 1)]);
            }
        }
        Diags diags;
        const double t0 = nowMs();
        auto prog = Program::parse(t, unit.files, diags);
        for (const char* name : {"QX::StateData", "QUOTTERY::StateData", "NOST::StateData", "m256i"}) {
            TypeId id = prog->lookupType(name);
            if (id != kNoType) (void)prog->layoutOf(id);
        }
        auto var = prog->lookupVariable("contractDescriptions");
        if (var) (void)prog->evalInitializer(*var);
        CHECK_MESSAGE(nowMs() - t0 < 5000, "iteration " << iter << " took too long");
    }
}

TEST_CASE("program: the LP64 / LLP64 choice for `long` does not influence any contract state size") {
    for (const char* name : {"v1.303.2.node.generic.ii", "HEAD.node.generic.ii"}) {
        Dataset* ds = iiDataset(name);
        if (!ds) { MESSAGE("QSTATE_TEST_II_DIR / " << std::string(name) << " missing, skipping"); continue; }
        IiUnit unit;
        REQUIRE(loadIi(qstate::testing::envOr("QSTATE_TEST_II_DIR") + "/" + name, unit));
        ProgramOptions options;
        options.longIs64 = true;
        Diags diags;
        auto lp64 = Program::parse(std::move(unit.tokens), std::move(unit.files), diags, options);
        std::vector<ContractRow> a = contractRows(*ds->prog);
        std::vector<ContractRow> b = contractRows(*lp64);
        REQUIRE(a.size() == b.size());
        for (std::size_t i = 0; i < a.size(); ++i) CHECK_MESSAGE(a[i].stateSize == b[i].stateSize, std::string(name) << " " << a[i].assetName);
        CHECK(lp64->evalConstant("sizeof(long)").value().value == 8);
        CHECK(ds->prog->evalConstant("sizeof(long)").value().value == 4);
    }
}

TEST_CASE("program: m256i and uint128_t come from the headers (no special cases)") {
    Dataset* ds = iiDataset("HEAD.node.generic.ii");
    if (!ds) { MESSAGE("QSTATE_TEST_II_DIR missing, skipping"); return; }
    Program& prog = *ds->prog;
    TypeId id = prog.lookupType("m256i");
    REQUIRE(id != kNoType);
    const TypeLayout& l = prog.layoutOf(id);
    CHECK(l.kind == TypeKind::Record);
    CHECK(l.recordKind == RecordKind::Union);
    CHECK(l.size == 32);
    CHECK(l.align == 8);
    TypeId qid = prog.lookupType("QPI::id");
    CHECK(qid == id);
    TypeId u128 = prog.lookupType("uint128_t");
    REQUIRE(u128 != kNoType);
    CHECK(prog.layoutOf(u128).size == 16);
    CHECK(prog.layoutOf(u128).align == 8);
    // canonical identity and display names
    TypeId c1 = prog.lookupType("QPI::Collection<QX::AssetOrder, 2097152>");
    TypeId c2 = prog.lookupType("Collection<QX::AssetOrder,2097152>");
    REQUIRE(c1 != kNoType);
    CHECK(c1 == c2);
    CHECK(prog.typeName(c1) == "QPI::Collection<QX::AssetOrder, 2097152>");
    const TypeLayout& cl = prog.layoutOf(c1);
    CHECK(cl.templateName == "QPI::Collection");
    REQUIRE(cl.templateArgs.size() == 2);
    CHECK(cl.templateArgs[1].text == "2097152");
    CHECK(cl.size == 302514192);
}

TEST_CASE("program: evalConstant with sizeof of real state types") {
    Dataset* ds = iiDataset("v1.303.2.node.generic.ii");
    if (!ds) { MESSAGE("QSTATE_TEST_II_DIR missing, skipping"); return; }
    Program& prog = *ds->prog;
    auto v = prog.evalConstant("sizeof(QX::StateData)");
    REQUIRE(v.has_value());
    CHECK(v->value == 621806120);
    auto w = prog.evalConstant("sizeof(QX::StateData) / sizeof(QPI::id) + NUMBER_OF_COMPUTORS", "");
    CHECK(!w.has_value()); // NUMBER_OF_COMPUTORS is a macro: not visible after preprocessing
    auto x = prog.evalConstant("sizeof(AssetOrder)", "QX");
    REQUIRE(x.has_value());
    CHECK(x->value == 40);
}

} // namespace
