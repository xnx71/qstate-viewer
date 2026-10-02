#include "commands.h"
#include "format.h"
#include "qstate/service/core_loader.h"
#include "qstate/service/schema_json.h"
#include "session.h"

#include <algorithm>
#include <ostream>
#include <set>

namespace qstate::cli {

using nlohmann::json;

namespace {

void collectTypes(const schema::Schema& s, schema::TypeId id, std::set<schema::TypeId>& seen) {
    if (id == schema::kNoType || id >= s.types.size() || !seen.insert(id).second) return;
    const schema::Type& t = s.types[id];
    collectTypes(s, t.underlying, seen);
    collectTypes(s, t.element, seen);
    for (const schema::Field& f : t.fields) collectTypes(s, f.type, seen);
    for (const schema::BaseClass& b : t.bases) collectTypes(s, b.type, seen);
    collectTypes(s, t.role.element, seen);
    collectTypes(s, t.role.key, seen);
    collectTypes(s, t.role.value, seen);
}

json contractJson(const schema::ContractSchema& c) {
    json j = {{"index", c.index}, {"name", c.name}, {"constructionEpoch", c.constructionEpoch}, {"expectedSize", c.expectedSize}};
    if (!c.structName.empty()) j["structName"] = c.structName;
    if (!c.stateTypeName.empty()) j["stateTypeName"] = c.stateTypeName;
    if (c.stateType != schema::kNoType) j["stateTypeId"] = c.stateType;
    if (!c.headerFile.empty()) j["headerFile"] = c.headerFile;
    if (c.destructionEpoch != 0) j["destructionEpoch"] = c.destructionEpoch;
    if (!c.error.empty()) j["error"] = c.error;
    return j;
}

void printFields(const schema::Schema& s, const schema::Type& t, std::uint64_t base, int depth, int level, TextTable& table) {
    for (const schema::BaseClass& b : t.bases) {
        table.rows.push_back({hexOffset(base + b.offset), "", std::string(level * 2, ' ') + b.typeName, "(base class)"});
    }
    for (const schema::Field& f : t.fields) {
        std::string type = f.typeName;
        if (f.bitWidth > 0) type += " : " + std::to_string(f.bitWidth) + " (bit " + std::to_string(f.bitOffset) + ")";
        table.rows.push_back({hexOffset(base + f.offset), std::to_string(f.size), std::string(level * 2, ' ') + type, f.name});
        if (depth > 1 && f.type < s.types.size()) {
            const schema::Type& ft = s.types[f.type];
            if (ft.kind == schema::TypeKind::Record && ft.role.kind == schema::RoleKind::None) {
                printFields(s, ft, base + f.offset, depth - 1, level + 1, table);
            }
        }
    }
}

} // namespace

int cmdSchema(const Args& args, std::ostream& out, std::ostream& err) {
    service::CoreRequest request;
    request.coreDir = args.require("core");
    request.coreRef = args.get("ref").value_or("");
    if (auto epoch = args.integer("epoch")) request.epoch = static_cast<int>(*epoch);
    request.defines = args.all("define");
    const std::optional<long long> only = args.integer("contract");
    const long long depth = args.integer("depth").value_or(1);
    if (depth < 1 || depth > 16) throw UsageError("--depth must be between 1 and 16");

    service::LoadedCore core;
    try {
        core = service::loadCore(request);
    } catch (const rpc::Error& e) {
        throw RpcFailure(std::string(rpc::codeName(e.code())), e.what());
    }
    const schema::Schema& s = *core.schema;
    const schema::ContractSchema* selected = nullptr;
    if (only) {
        selected = s.contract(static_cast<std::uint32_t>(*only));
        if (selected == nullptr) throw RpcFailure("not_found", "the core sources define no contract with index " + std::to_string(*only));
    }
    int exitCode = 0;
    for (const schema::ContractSchema& c : s.contracts) {
        if (!c.error.empty() && (selected == nullptr || selected == &c)) exitCode = 1;
    }

    if (args.flag("json")) {
        json j;
        j["core"] = core.info;
        json diags = json::array();
        for (const auto& d : core.diagnostics) diags.push_back(d);
        j["diagnostics"] = std::move(diags);
        if (selected) {
            j["contract"] = contractJson(*selected);
            std::set<schema::TypeId> ids;
            collectTypes(s, selected->stateType, ids);
            json types = json::array();
            for (schema::TypeId id : ids) types.push_back(service::typeInfoJson(s.types[id]));
            j["types"] = std::move(types);
        } else {
            json contracts = json::array();
            for (const auto& c : s.contracts) contracts.push_back(contractJson(c));
            j["contracts"] = std::move(contracts);
        }
        out << j.dump(2) << "\n";
        return exitCode;
    }

    out << "core: " << core.info.sourceDir << "  (" << (core.info.ref.empty() ? std::string("working tree") : "ref " + core.info.ref);
    if (!core.info.version.empty()) out << ", version " << core.info.version;
    if (core.info.epoch) out << ", epoch " << *core.info.epoch;
    out << "; " << core.info.fileCount << " files, " << static_cast<long>(core.info.parseMs) << " ms)\n";
    for (const auto& d : core.diagnostics) {
        out << (d.severity == service::Diagnostic::Severity::Error ? "error" : d.severity == service::Diagnostic::Severity::Warning ? "warning" : "note")
            << ": " << d.message << (d.file.empty() ? "" : "  (" + d.file + (d.line ? ":" + std::to_string(d.line) : "") + ")") << "\n";
    }
    out << "\n";
    if (selected == nullptr) {
        TextTable table;
        table.header = {"IDX", "NAME", "STATE TYPE", "SIZE", "CONSTR.EPOCH", "HEADER"};
        table.rightAlign = {true, false, false, true, true, false};
        for (const auto& c : s.contracts) {
            table.rows.push_back({std::to_string(c.index), c.name, c.stateTypeName, c.stateType == schema::kNoType ? "?" : std::to_string(c.expectedSize),
                                  std::to_string(c.constructionEpoch), c.headerFile});
        }
        table.print(out);
        for (const auto& c : s.contracts) {
            if (!c.error.empty()) out << "  contract " << c.index << " " << c.name << ": " << c.error << "\n";
        }
        out << s.contracts.size() << " contracts, " << s.types.size() << " types\n";
        return exitCode;
    }
    out << "contract " << selected->index << " " << (selected->name.empty() ? "(contract 0)" : selected->name) << ": " << selected->stateTypeName;
    if (selected->stateType == schema::kNoType) {
        out << "\n  layout not available: " << selected->error << "\n";
        return 1;
    }
    const schema::Type& st = s.types[selected->stateType];
    out << "  size " << st.size << "  align " << st.align;
    if (st.source) out << "  (" << st.source->file << ":" << st.source->line << ")";
    out << "\n\n";
    TextTable table;
    table.header = {"OFFSET", "SIZE", "TYPE", "NAME"};
    table.rightAlign = {true, true, false, false};
    printFields(s, st, 0, static_cast<int>(depth), 0, table);
    table.print(out);
    (void)err;
    return exitCode;
}

} // namespace qstate::cli
