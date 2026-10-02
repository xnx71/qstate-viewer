// Layout engine: size / alignment / member offsets of records (MSVC x86-64 rules), enums and arrays; static_assert
// checking.
#include <algorithm>
#include <climits>

#include "program_impl.h"

namespace qstate::cpp {
namespace {

std::uint64_t alignUp(std::uint64_t v, std::uint64_t a) { return a <= 1 ? v : (v + a - 1) / a * a; }

constexpr std::uint64_t kMaxSize = 1ull << 62;

// Nested "incomplete type" messages would otherwise grow with the nesting depth.
std::string cap(const std::string& s) { return s.size() <= 240 ? s : s.substr(0, 240) + "..."; }

// Value of alignas(expr) / alignas(type).
std::uint32_t explicitAlign(Program::Impl& I, Scope sc, TokRange r) {
    Sema s(I, sc, r);
    if (auto t = s.tryParseTypeId(); t && s.atEnd()) return I.layoutOf(*t).align;
    Sema e(I, sc, r);
    ConstValue v = e.parseExpr();
    e.expectEnd();
    if (v.value <= 0 || (v.value & (v.value - 1)) != 0 || v.value > (1 << 16)) throw SemaError("invalid alignment");
    return static_cast<std::uint32_t>(v.value);
}

// Maximum member alignment from "#pragma pack" at the definition of `d` (0 = none).
std::uint32_t packFor(const Program::Impl& I, const Decl* d) {
    const auto& regions = I.opts.packRegions;
    if (regions.empty()) return 0;
    auto it = std::upper_bound(regions.begin(), regions.end(), d->srcTok,
                               [](std::uint32_t tok, const PackRegion& r) { return tok < r.beginToken; });
    if (it == regions.begin()) return 0;
    --it;
    return d->srcTok < it->endToken ? it->pack : 0;
}

struct StateGuard {
    TypeInfo& ti;
    ~StateGuard() { ti.state = TypeInfo::State::Done; }
};

} // namespace

const TypeLayout& Program::Impl::layoutOf(TypeId id) {
    static const TypeLayout kInvalid = [] {
        TypeLayout l;
        l.error = "invalid type id";
        return l;
    }();
    if (id >= types.size()) return kInvalid;
    TypeInfo& ti = types[id];
    if (ti.state == TypeInfo::State::Done) return ti.layout;
    if (ti.state == TypeInfo::State::InProgress) {
        // recursion: the type needs itself by value
        static thread_local TypeLayout recursive;
        recursive = ti.layout;
        recursive.complete = false;
        recursive.error = "type '" + ti.layout.name + "' is incomplete (recursive by-value use)";
        return recursive;
    }
    computeLayout(id);
    return types[id].layout;
}

void Program::Impl::computeLayout(TypeId id) {
    TypeInfo& ti = types[id];
    ti.state = TypeInfo::State::InProgress;
    StateGuard guard{ti};
    struct LayoutDepth {
        int& d;
        explicit LayoutDepth(int& x) : d(x) { ++d; }
        ~LayoutDepth() { --d; }
    } depthGuard(this->layoutDepth);
    try {
        if (this->layoutDepth > opts.maxInstantiationDepth + 64) throw SemaError("type nesting too deep ('" + ti.layout.name + "')");
        switch (ti.layout.kind) {
        case TypeKind::Array: {
            const TypeLayout el = layoutOf(ti.layout.element);
            if (!el.complete) {
                ti.layout.complete = false;
                ti.layout.error = "array element type '" + el.name + "' is incomplete" + (el.error.empty() ? "" : ": " + cap(el.error));
                return;
            }
            const std::uint64_t n = ti.layout.count;
            if (el.size != 0 && n > kMaxSize / el.size) throw SemaError("array '" + ti.layout.name + "' is too large");
            ti.layout.size = el.size * n;
            ti.layout.align = el.align;
            ti.layout.complete = true;
            break;
        }
        case TypeKind::Record:
            computeRecord(id);
            break;
        case TypeKind::Enum:
            computeEnum(id);
            break;
        default:
            ti.layout.complete = true;
            break;
        }
    } catch (const SemaError& ex) {
        TypeInfo& t2 = types[id];
        t2.layout.complete = false;
        t2.layout.error = ex.what();
        diagAt(Diag::Severity::Warning, "layout of '" + t2.layout.name + "' failed: " + ex.what(), t2.layout.loc.file, t2.layout.loc.line);
    }
}

void Program::Impl::computeEnum(TypeId id) {
    TypeInfo& ti = types[id];
    const Decl* d = ti.decl;
    const Env* env = ti.env;
    TypeId under = kNoType;
    if (d->hasUnderlying) {
        Sema s(*this, Scope{d->parent, env ? env->outer : nullptr}, d->underlying);
        under = s.parseTypeId();
        s.expectEnd();
    }
    std::vector<EnumeratorInfo> list;
    Scope sc{d, env};
    bool fitsInt = true;
    bool fitsUInt = true;
    const Env* declEnv = env;
    for (const Decl* m : d->members) {
        if (m->kind != DeclKind::Enumerator) continue;
        ConstValue v = enumeratorValue(m, declEnv);
        list.push_back(EnumeratorInfo{text(m->name), v.value});
        if (v.value < INT32_MIN || v.value > INT32_MAX) fitsInt = false;
        if (v.value < 0 || v.value > static_cast<std::int64_t>(UINT32_MAX)) fitsUInt = false;
        if (v.bits == 64 && !v.isSigned && v.value < 0) { fitsInt = false; fitsUInt = false; }
    }
    (void)sc;
    TypeInfo& t2 = types[id];
    if (under == kNoType) {
        if (fitsInt) under = tInt;
        else if (fitsUInt) under = tUInt;
        else under = tLongLong;
    }
    const TypeLayout& ul = layoutOf(under);
    t2.layout.underlying = under;
    t2.layout.size = ul.size;
    t2.layout.align = ul.align;
    t2.layout.isSigned = ul.isSigned;
    t2.layout.enumerators = std::move(list);
    t2.layout.complete = d->defined || d->hasUnderlying;
    if (!t2.layout.complete) t2.layout.error = "enum is not defined";
}

void Program::Impl::computeRecord(TypeId id) {
    TypeInfo& ti = types[id];
    const Decl* d = ti.decl;
    const Env* env = ti.env;
    if (!d->defined) {
        ti.layout.complete = false;
        ti.layout.error = "incomplete type (no definition found)";
        diagAt(Diag::Severity::Note, "incomplete type '" + ti.layout.name + "'", d->file, d->line);
        return;
    }
    const bool isUnion = d->recordKind == RecordKind::Union;
    Scope sc{d, env};
    const std::uint32_t pack = packFor(*this, d);
    std::uint64_t offset = 0; // end of the data laid out so far (struct) / max size (union)
    std::uint32_t align = 1;
    bool hasData = false;
    std::vector<FieldLayout> fields;
    std::vector<BaseLayout> bases;
    std::string error;

    // bases (copy: resolving them may create types)
    const std::vector<TypeId> baseIds = basesOf(id);
    if (baseIds.size() != d->bases.size()) error = "unresolved base class";
    for (TypeId b : baseIds) {
        const TypeLayout bl = layoutOf(b);
        if (!bl.complete) {
            error = "base class '" + bl.name + "' is incomplete" + (bl.error.empty() ? "" : ": " + cap(bl.error));
            break;
        }
        BaseLayout out;
        out.type = b;
        const std::uint32_t balign = pack != 0 ? std::min(bl.align, pack) : bl.align;
        if (bl.isEmpty) {
            out.offset = 0;
        } else {
            out.offset = alignUp(offset, balign);
            offset = out.offset + bl.size;
            align = std::max(align, balign);
            hasData = true;
        }
        bases.push_back(out);
    }

    // bit-field allocation state (MSVC): one open storage unit of a given size
    bool unitOpen = false;
    std::uint64_t unitStart = 0;
    std::uint32_t unitSize = 0;
    std::uint32_t unitUsed = 0;

    if (error.empty()) {
        for (const Decl* m : d->members) {
            if (m->kind == DeclKind::StaticAssert) {
                if (env) {
                    try {
                        Sema s(*this, sc, m->expr);
                        ConstValue v = s.parseExpr();
                        if (v.value == 0)
                            diagAt(Diag::Severity::Warning, "static_assert failed in '" + ti.layout.name + "': " + rangeText(m->expr), m->file, m->line);
                    } catch (const SemaError& ex) {
                        diagAt(Diag::Severity::Note, "static_assert in '" + ti.layout.name + "' not evaluated: " + ex.what(), m->file, m->line);
                    }
                }
                continue;
            }
            if (m->kind != DeclKind::Field || m->isStatic) continue;
            TypeId ft;
            try {
                ft = resolveTypeSpec(m->type, sc, m);
            } catch (const SemaError& ex) {
                error = "field '" + (m->name ? text(m->name) : std::string("<anonymous>")) + "': " + ex.what();
                break;
            }
            const TypeLayout fl = layoutOf(ft);
            if (!fl.complete) {
                error = "field '" + (m->name ? text(m->name) : std::string("<anonymous>")) + "' has incomplete type '" + fl.name + "'" + (fl.error.empty() ? "" : ": " + cap(fl.error));
                break;
            }
            FieldLayout f;
            f.name = m->name ? text(m->name) : std::string();
            f.type = ft;
            f.size = fl.size;
            f.align = fl.align;
            if (pack != 0 && f.align > pack) f.align = pack;
            if (!m->alignExpr.empty()) {
                try {
                    f.align = std::max<std::uint32_t>(f.align, explicitAlign(*this, sc, m->alignExpr));
                } catch (const SemaError& ex) {
                    error = std::string("alignas: ") + ex.what();
                    break;
                }
            }
            f.loc = SourceLoc{m->file, m->line};
            // declared spelling
            {
                std::string decl;
                if (m->type.inlineDecl) decl = types[ft].layout.name;
                else decl = rangeText(m->type.tokens);
                decl.append(m->type.ptrDepth, '*');
                if (m->type.isRef) decl += "&";
                for (const TokRange& r : m->type.extents) decl += "[" + rangeText(r) + "]";
                if (m->type.unsized) decl += "[]";
                f.declaredType = std::move(decl);
            }
            hasData = true;
            if (m->isBitField) {
                ConstValue w;
                try {
                    Sema s(*this, sc, m->bitWidth);
                    w = s.parseExpr();
                } catch (const SemaError& ex) {
                    error = "bit-field width: " + std::string(ex.what());
                    break;
                }
                const std::uint32_t width = static_cast<std::uint32_t>(std::max<std::int64_t>(0, w.value));
                if (width == 0) {
                    unitOpen = false;
                    offset = alignUp(offset, f.align);
                    continue;
                }
                f.bitWidth = width;
                if (isUnion) {
                    f.offset = 0;
                    f.bitOffset = 0;
                    offset = std::max<std::uint64_t>(offset, fl.size);
                } else if (unitOpen && unitSize == fl.size && unitUsed + width <= unitSize * 8) {
                    f.offset = unitStart;
                    f.bitOffset = unitUsed;
                    unitUsed += width;
                } else {
                    unitStart = alignUp(offset, f.align);
                    unitSize = static_cast<std::uint32_t>(fl.size);
                    unitUsed = width;
                    unitOpen = true;
                    f.offset = unitStart;
                    f.bitOffset = 0;
                    offset = unitStart + unitSize;
                }
                align = std::max(align, f.align);
                fields.push_back(std::move(f));
                continue;
            }
            unitOpen = false;
            if (isUnion) {
                f.offset = 0;
                offset = std::max<std::uint64_t>(offset, fl.size);
            } else {
                f.offset = alignUp(offset, f.align);
                if (fl.size > kMaxSize || f.offset > kMaxSize - fl.size) {
                    error = "record '" + ti.layout.name + "' is too large";
                    break;
                }
                offset = f.offset + fl.size;
            }
            align = std::max(align, f.align);
            fields.push_back(std::move(f));
        }
    }

    if (error.empty() && !d->alignExpr.empty()) {
        try {
            align = std::max<std::uint32_t>(align, explicitAlign(*this, sc, d->alignExpr));
        } catch (const SemaError& ex) {
            error = std::string("alignas: ") + ex.what();
        }
    }
    TypeInfo& t2 = types[id];
    TypeLayout& L = t2.layout;
    L.fields = std::move(fields);
    L.bases = std::move(bases);
    L.align = align;
    L.isEmpty = !hasData;
    L.size = hasData ? alignUp(offset, align) : 1;
    if (!error.empty()) {
        L.complete = false;
        L.error = error;
        diagAt(Diag::Severity::Warning, "layout of '" + L.name + "' failed: " + error, d->file, d->line);
        return;
    }
    L.complete = true;
}

// ---- static_assert --------------------------------------------------------------------------------------------

void Program::Impl::checkStaticAssertsIn(const Decl* scope) {
    for (const Decl* m : scope->members) {
        switch (m->kind) {
        case DeclKind::StaticAssert: {
            try {
                Sema s(*this, Scope{scope, nullptr}, m->expr);
                ConstValue v = s.parseExpr();
                if (v.value == 0)
                    diagAt(Diag::Severity::Warning, "static_assert failed: " + rangeText(m->expr), m->file, m->line);
            } catch (const SemaError& ex) {
                diagAt(Diag::Severity::Note, std::string("static_assert not evaluated: ") + ex.what(), m->file, m->line);
            }
            break;
        }
        case DeclKind::Namespace:
            checkStaticAssertsIn(m);
            break;
        case DeclKind::Record:
            if (!m->isTemplate && !m->isSpecialization && m->defined) checkStaticAssertsIn(m);
            break;
        default:
            break;
        }
    }
}

void Program::Impl::checkStaticAsserts() { checkStaticAssertsIn(global); }

} // namespace qstate::cpp
