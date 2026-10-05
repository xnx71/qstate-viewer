#include "qstate/schema/extract.h"

#include <chrono>
#include <algorithm>
#include <regex>
#include <unordered_map>

#include "qstate/cpp/preprocessor.h"
#include "qstate/cpp/program.h"

namespace qstate::schema {
namespace {

using qstate::cpp::Diag;
using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

// Converts cpp::TypeLayout graphs into schema::Type entries, deduplicated by cpp::TypeId.
class Converter {
public:
    Converter(qstate::cpp::Program& program, Schema& schema) : program_(program), schema_(schema) {}

    TypeId convert(qstate::cpp::TypeId id) {
        if (id == qstate::cpp::kNoType) return kNoType;
        if (auto it = map_.find(id); it != map_.end()) return it->second;

        // Copy: layoutOf references stay valid, but the recursion below may create further layouts.
        const qstate::cpp::TypeLayout layout = program_.layoutOf(id);

        const TypeId mine = static_cast<TypeId>(schema_.types.size());
        schema_.types.emplace_back();
        map_[id] = mine;

        Type t;
        t.id = mine;
        t.name = layout.name;
        t.size = layout.size;
        t.align = layout.align ? layout.align : 1;
        if (!layout.complete) {
            schema_.diags.push_back({Diag::Severity::Warning, "incomplete type " + layout.name + ": " + layout.error, {}, 0});
        }
        if (layout.loc.line != 0) t.source = SourceLoc{program_.fileName(layout.loc.file), layout.loc.line};

        using K = qstate::cpp::TypeKind;
        switch (layout.kind) {
        case K::Bool:
            t.kind = TypeKind::Prim;
            t.prim = PrimKind::Bool;
            break;
        case K::Char:
            t.kind = TypeKind::Prim;
            t.prim = PrimKind::Char;
            t.isSigned = layout.isSigned;
            break;
        case K::Int:
            t.kind = TypeKind::Prim;
            t.prim = layout.isSigned ? PrimKind::SInt : PrimKind::UInt;
            t.isSigned = layout.isSigned;
            break;
        case K::Float:
            t.kind = TypeKind::Prim;
            t.prim = PrimKind::Float;
            t.isSigned = true;
            break;
        case K::Void:
        case K::NullPtr:
            t.kind = TypeKind::Prim;
            t.prim = PrimKind::UInt;
            break;
        case K::Pointer:
            t.kind = TypeKind::Pointer;
            t.element = convert(layout.element);
            break;
        case K::Array:
            t.kind = TypeKind::Array;
            t.element = convert(layout.element);
            t.count = layout.count;
            break;
        case K::Enum:
            t.kind = TypeKind::Enum;
            t.isSigned = layout.isSigned;
            t.underlying = convert(layout.underlying);
            for (const auto& e : layout.enumerators) t.enumerators.push_back({e.name, e.value});
            break;
        case K::Record:
            t.kind = TypeKind::Record;
            switch (layout.recordKind) {
            case qstate::cpp::RecordKind::Struct: t.recordKind = RecordKind::Struct; break;
            case qstate::cpp::RecordKind::Class: t.recordKind = RecordKind::Class; break;
            case qstate::cpp::RecordKind::Union: t.recordKind = RecordKind::Union; break;
            }
            break;
        }

        if (layout.kind == K::Record) {
            int anon = 0;
            for (const auto& f : layout.fields) {
                Field out;
                out.name = f.name.empty() ? "$anon" + std::to_string(anon++) : f.name;
                out.type = convert(f.type);
                out.typeName = f.declaredType.empty() ? program_.typeName(f.type) : f.declaredType;
                out.offset = f.offset;
                out.size = f.size;
                out.bitOffset = f.bitOffset;
                out.bitWidth = f.bitWidth;
                t.fields.push_back(std::move(out));
            }
            for (const auto& b : layout.bases) {
                BaseClass out;
                out.type = convert(b.type);
                out.typeName = program_.typeName(b.type);
                out.offset = b.offset;
                t.bases.push_back(std::move(out));
            }
            if (!layout.templateName.empty()) {
                TemplateInfo ti;
                ti.name = layout.templateName;
                for (const auto& a : layout.templateArgs) ti.args.push_back(a.text);
                t.templateInfo = std::move(ti);
            }
            t.role = recognizeRole(layout);
        }

        schema_.types[mine] = std::move(t);
        return mine;
    }

private:
    Role recognizeRole(const qstate::cpp::TypeLayout& l) {
        Role r;
        const std::string& tn = l.templateName;
        const auto& a = l.templateArgs;
        auto typeArg = [&](std::size_t i) { return i < a.size() && a[i].isType ? convert(a[i].type) : kNoType; };
        auto valueArg = [&](std::size_t i) {
            return i < a.size() && !a[i].isType ? static_cast<std::uint64_t>(a[i].value) : std::uint64_t{0};
        };

        if (!tn.empty()) {
            if (tn == "QPI::Array" || tn == "QPI::SlowAnySizeArray") {
                r.kind = RoleKind::Array;
                r.element = typeArg(0);
                r.capacity = valueArg(1);
            } else if (tn == "QPI::BitArray") {
                r.kind = RoleKind::BitArray;
                r.capacity = valueArg(0);
            } else if (tn == "QPI::HashMap") {
                r.kind = RoleKind::HashMap;
                r.key = typeArg(0);
                r.value = typeArg(1);
                r.capacity = valueArg(2);
            } else if (tn == "QPI::HashSet") {
                r.kind = RoleKind::HashSet;
                r.key = typeArg(0);
                r.capacity = valueArg(1);
            } else if (tn == "QPI::Collection") {
                r.kind = RoleKind::Collection;
                r.element = typeArg(0);
                r.capacity = valueArg(1);
            } else if (tn == "QPI::LinkedList") {
                r.kind = RoleKind::LinkedList;
                r.element = typeArg(0);
                r.capacity = valueArg(1);
            }
            return r;
        }
        // Non-template QPI value types, identified by their declaration (name + size so a look-alike elsewhere
        // cannot be mistaken for them).
        if (l.baseName == "m256i" && l.size == 32) r.kind = RoleKind::Id;
        else if (l.name == "QPI::bit" && l.size == 1) r.kind = RoleKind::Bit;
        else if (l.baseName == "uint128_t" && l.size == 16) r.kind = RoleKind::Uint128;
        else if (l.name == "QPI::DateAndTime" && l.size == 8) r.kind = RoleKind::DateTime;
        return r;
    }

    qstate::cpp::Program& program_;
    Schema& schema_;
    std::unordered_map<qstate::cpp::TypeId, TypeId> map_;
};

struct TableRow {
    std::string assetName;
    std::uint32_t construction = 0;
    std::uint32_t destruction = 0;
    std::string stateType; // spelling inside sizeof(...)
    std::uint64_t stateSize = 0;
    bool sizeKnown = false;
};

// Rows of contractDescriptions[]: values come from the evaluated initializer, the state type name from the token
// text (the evaluator folds sizeof(T) into a number).
std::vector<TableRow> readTable(qstate::cpp::Program& program, const std::string& tableName, Schema& schema) {
    std::vector<TableRow> rows;
    auto var = program.lookupVariable(tableName);
    if (!var || !var->hasInitializer) {
        schema.diags.push_back({Diag::Severity::Error, "table '" + tableName + "' not found in the translation unit", {}, 0});
        return rows;
    }
    auto init = program.evalInitializer(*var);
    const std::string text = program.tokenText(var->initBegin, var->initEnd);
    if (!init || init->kind != qstate::cpp::InitValue::Kind::List) {
        schema.diags.push_back({Diag::Severity::Error, "cannot evaluate table '" + tableName + "'", {}, 0});
        return rows;
    }

    // `{ "QX" , 66 , 10000 , sizeof ( QX :: StateData ) }`: spacing is normalised by tokenText.
    static const std::regex rowRe(R"RE(\{\s*"([^"]*)"\s*,[^,]*,[^,]*,\s*sizeof\s*\(\s*([A-Za-z0-9_:\s]+?)\s*\)\s*\})RE");
    std::vector<std::string> types;
    for (std::sregex_iterator it(text.begin(), text.end(), rowRe), end; it != end; ++it) {
        std::string ty = (*it)[2].str();
        ty.erase(std::remove(ty.begin(), ty.end(), ' '), ty.end());
        types.push_back(std::move(ty));
    }
    if (types.size() != init->items.size()) {
        schema.diags.push_back({Diag::Severity::Error,
                                "table '" + tableName + "': found " + std::to_string(types.size()) +
                                    " state type names for " + std::to_string(init->items.size()) + " rows",
                                {}, 0});
        return rows;
    }

    for (std::size_t i = 0; i < init->items.size(); ++i) {
        const auto& row = init->items[i];
        TableRow r;
        r.stateType = types[i];
        if (row.kind != qstate::cpp::InitValue::Kind::List || row.items.size() < 4) {
            schema.diags.push_back({Diag::Severity::Error, "table row " + std::to_string(i) + " is not evaluable", {}, 0});
            rows.push_back(std::move(r));
            continue;
        }
        r.assetName = row.items[0].s;
        r.construction = static_cast<std::uint32_t>(row.items[1].i);
        r.destruction = static_cast<std::uint32_t>(row.items[2].i);
        r.sizeKnown = row.items[3].kind == qstate::cpp::InitValue::Kind::Int;
        r.stateSize = r.sizeKnown ? static_cast<std::uint64_t>(row.items[3].i) : 0;
        rows.push_back(std::move(r));
    }
    return rows;
}

} // namespace

ExtractResult extractSchema(const qstate::cpp::SourceProvider& source, const ExtractOptions& options) {
    const auto t0 = Clock::now();
    ExtractResult result;
    result.schema = std::make_shared<Schema>();
    Schema& schema = *result.schema;

    qstate::cpp::PreprocessOptions po;
    po.includeDirs = options.includeDirs;
    po.defines = options.defines;
    qstate::cpp::Preprocessor pp(source, po);

    auto tp = Clock::now();
    qstate::cpp::PreprocessResult pre = pp.run(options.rootFile);
    result.stats.preprocessMs = msSince(tp);
    result.stats.fileCount = pre.files.size();
    result.stats.tokenCount = pre.tokens.size();
    result.files = pre.files;
    for (const Diag& d : pre.diags) schema.diags.push_back(d);

    if (pre.files.empty() || pre.aborted) {
        schema.diags.push_back({Diag::Severity::Error, "cannot read " + options.rootFile, options.rootFile, 0});
        result.stats.totalMs = msSince(t0);
        return result;
    }

    tp = Clock::now();
    qstate::cpp::Diags parseDiags;
    qstate::cpp::ProgramOptions programOptions;
    programOptions.packRegions = qstate::cpp::packRegionsFromPragmas(pre.pragmas, static_cast<std::uint32_t>(pre.tokens.size()));
    std::unique_ptr<qstate::cpp::Program> program =
        qstate::cpp::Program::parse(std::move(pre.tokens), pre.files, parseDiags, programOptions);
    result.stats.parseMs = msSince(tp);
    for (const Diag& d : parseDiags) schema.diags.push_back(d);

    tp = Clock::now();
    std::vector<TableRow> rows = readTable(*program, options.tableName, schema);
    Converter conv(*program, schema);

    for (std::size_t i = 0; i < rows.size(); ++i) {
        const TableRow& row = rows[i];
        ContractSchema c;
        c.index = static_cast<std::uint32_t>(i);
        c.name = row.assetName;
        c.constructionEpoch = row.construction;
        c.destructionEpoch = row.destruction;
        c.stateTypeName = row.stateType;
        c.expectedSize = row.stateSize;

        const std::size_t sep = row.stateType.rfind("::");
        if (sep != std::string::npos) c.structName = row.stateType.substr(0, sep);

        const qstate::cpp::TypeId cppType = program->lookupType(row.stateType);
        if (cppType == qstate::cpp::kNoType) {
            c.error = "type " + row.stateType + " not found";
        } else {
            const qstate::cpp::TypeLayout& layout = program->layoutOf(cppType);
            if (!layout.complete) {
                c.error = "layout of " + row.stateType + " failed: " + layout.error;
            } else {
                c.stateType = conv.convert(cppType);
                if (layout.loc.line != 0) c.headerFile = program->fileName(layout.loc.file);
                if (row.sizeKnown && layout.size != row.stateSize) {
                    c.error = "sizeof(" + row.stateType + ") evaluates to " + std::to_string(row.stateSize) +
                              " in the table but the layout engine computed " + std::to_string(layout.size);
                }
                if (!row.sizeKnown) c.expectedSize = layout.size;
            }
        }
        if (!c.error.empty()) {
            schema.diags.push_back({Diag::Severity::Error, "contract " + std::to_string(i) + " (" + c.name + "): " + c.error,
                                    c.headerFile, 0});
        }
        schema.contracts.push_back(std::move(c));
    }
    for (const Diag& d : program->diags()) {
        // Parse diagnostics were already copied; only add late ones created by the layout queries.
        bool known = false;
        for (const Diag& e : schema.diags)
            if (e.message == d.message && e.line == d.line) { known = true; break; }
        if (!known) schema.diags.push_back(d);
    }
    result.stats.layoutMs = msSince(tp);
    result.stats.totalMs = msSince(t0);
    return result;
}

} // namespace qstate::schema
