// Types, name lookup, template instantiation and the lazily evaluated declarations (typedefs, variables, enums).
#include <algorithm>
#include <cctype>
#include <climits>

#include "program_impl.h"

namespace qstate::cpp {
namespace {

constexpr std::uint8_t kMaskType = 1;      // Record, Enum, Typedef
constexpr std::uint8_t kMaskNamespace = 2; // Namespace
constexpr std::uint8_t kMaskValue = 4;     // Variable, Enumerator, Field

bool declMatches(const Decl* d, std::uint8_t mask) {
    switch (d->kind) {
    case DeclKind::Record:
    case DeclKind::Enum:
    case DeclKind::Typedef:
        return (mask & kMaskType) != 0;
    case DeclKind::Namespace:
        return (mask & kMaskNamespace) != 0;
    case DeclKind::Variable:
    case DeclKind::Enumerator:
    case DeclKind::Field:
    case DeclKind::Function:
        return (mask & kMaskValue) != 0;
    default:
        return false;
    }
}

struct DepthGuard {
    int& depth;
    explicit DepthGuard(int& d) : depth(d) { ++depth; }
    ~DepthGuard() { --depth; }
};

} // namespace

// ---- builtin types -------------------------------------------------------------------------------------------

TypeId Program::Impl::newType(TypeKind kind) {
    types.emplace_back();
    TypeInfo& t = types.back();
    t.layout.id = static_cast<TypeId>(types.size() - 1);
    t.layout.kind = kind;
    return t.layout.id;
}

TypeId Program::Impl::addBuiltin(TypeKind kind, std::uint32_t size, bool isSigned, const char* name) {
    TypeId id = newType(kind);
    TypeInfo& t = types[id];
    t.layout.size = size;
    t.layout.align = size ? size : 1;
    t.layout.isSigned = isSigned;
    t.layout.name = name;
    t.layout.baseName = name;
    t.layout.complete = true;
    t.state = TypeInfo::State::Done;
    return id;
}

void Program::Impl::initBuiltins() {
    const std::uint32_t longSize = opts.longIs64 ? 8 : 4;
    tVoid = addBuiltin(TypeKind::Void, 0, false, "void");
    tBool = addBuiltin(TypeKind::Bool, 1, false, "bool");
    tChar = addBuiltin(TypeKind::Char, 1, true, "char");
    tSChar = addBuiltin(TypeKind::Char, 1, true, "signed char");
    tUChar = addBuiltin(TypeKind::Char, 1, false, "unsigned char");
    tShort = addBuiltin(TypeKind::Int, 2, true, "short");
    tUShort = addBuiltin(TypeKind::Int, 2, false, "unsigned short");
    tInt = addBuiltin(TypeKind::Int, 4, true, "int");
    tUInt = addBuiltin(TypeKind::Int, 4, false, "unsigned int");
    tLong = addBuiltin(TypeKind::Int, longSize, true, "long");
    tULong = addBuiltin(TypeKind::Int, longSize, false, "unsigned long");
    tLongLong = addBuiltin(TypeKind::Int, 8, true, "long long");
    tULongLong = addBuiltin(TypeKind::Int, 8, false, "unsigned long long");
    tFloat = addBuiltin(TypeKind::Float, 4, true, "float");
    tDouble = addBuiltin(TypeKind::Float, 8, true, "double");
    tLongDouble = addBuiltin(TypeKind::Float, 8, true, "long double");
    tWchar = addBuiltin(TypeKind::Char, opts.longIs64 ? 4 : 2, opts.longIs64, "wchar_t");
    tChar8 = addBuiltin(TypeKind::Char, 1, false, "char8_t");
    tChar16 = addBuiltin(TypeKind::Char, 2, false, "char16_t");
    tChar32 = addBuiltin(TypeKind::Char, 4, false, "char32_t");
    tNullPtr = addBuiltin(TypeKind::NullPtr, 8, false, "std::nullptr_t");
}

TypeId Program::Impl::pointerTo(TypeId t) {
    auto it = pointerCache.find(t);
    if (it != pointerCache.end()) return it->second;
    TypeId id = newType(TypeKind::Pointer);
    TypeInfo& ti = types[id];
    ti.layout.size = 8;
    ti.layout.align = 8;
    ti.layout.element = t;
    ti.layout.complete = true;
    ti.layout.name = types[t].layout.name + "*";
    ti.state = TypeInfo::State::Done;
    pointerCache.emplace(t, id);
    return id;
}

TypeId Program::Impl::arrayOf(TypeId t, std::uint64_t count) {
    auto key = std::make_pair(t, count);
    auto it = arrayCache.find(key);
    if (it != arrayCache.end()) return it->second;
    TypeId id = newType(TypeKind::Array);
    TypeInfo& ti = types[id];
    ti.layout.element = t;
    ti.layout.count = count;
    ti.layout.name = types[t].layout.name + "[" + std::to_string(count) + "]";
    arrayCache.emplace(key, id);
    return id;
}

bool Program::Impl::isIntegral(TypeId t) const {
    TypeKind k = types[t].layout.kind;
    return k == TypeKind::Int || k == TypeKind::Char || k == TypeKind::Bool || k == TypeKind::Enum;
}

ConstValue Program::Impl::normalize(std::int64_t v, std::uint8_t bits, bool isSigned) {
    ConstValue r;
    r.bits = bits;
    r.isSigned = isSigned;
    const std::uint64_t u = static_cast<std::uint64_t>(v);
    switch (bits) {
    case 1:
        r.value = v != 0;
        r.isSigned = false;
        break;
    case 8:
        r.value = isSigned ? static_cast<std::int64_t>(static_cast<std::int8_t>(u)) : static_cast<std::int64_t>(static_cast<std::uint8_t>(u));
        break;
    case 16:
        r.value = isSigned ? static_cast<std::int64_t>(static_cast<std::int16_t>(u)) : static_cast<std::int64_t>(static_cast<std::uint16_t>(u));
        break;
    case 32:
        r.value = isSigned ? static_cast<std::int64_t>(static_cast<std::int32_t>(u)) : static_cast<std::int64_t>(static_cast<std::uint32_t>(u));
        break;
    default:
        r.bits = 64;
        r.value = v;
        break;
    }
    return r;
}

ConstValue Program::Impl::convertValue(const ConstValue& v, TypeId t) const {
    const TypeInfo* ti = &types[t];
    if (ti->layout.kind == TypeKind::Enum) {
        if (ti->layout.underlying != kNoType) ti = &types[ti->layout.underlying];
        else return normalize(v.value, 32, true);
    }
    switch (ti->layout.kind) {
    case TypeKind::Bool:
        return normalize(v.value != 0, 1, false);
    case TypeKind::Char:
    case TypeKind::Int:
        return normalize(v.value, static_cast<std::uint8_t>(ti->layout.size * 8), ti->layout.isSigned);
    default:
        return v;
    }
}

// ---- display names ------------------------------------------------------------------------------------------

std::string Program::Impl::renderArg(const TemplateArg& a) {
    if (a.isType) return types[a.type].layout.name;
    if (a.value.bits == 1) return a.value.value ? "true" : "false";
    if (!a.value.isSigned) return std::to_string(a.value.asUnsigned());
    return std::to_string(a.value.value);
}

std::string Program::Impl::scopeDisplay(const Decl* d, const Env* e) {
    std::string prefix;
    const Decl* p = d->parent;
    if (p && p != global) {
        const Env* pe = e ? e->outer : nullptr;
        prefix = scopeDisplay(p, pe) + "::";
    }
    std::string nm = d->name != 0 ? text(d->name) : std::string("<anonymous>");
    if (e && e->decl == d && d->isTemplate && d->kind == DeclKind::Record) {
        const std::vector<TemplateArg>& shown = d->isSpecialization ? e->primaryArgs : e->args;
        nm += "<";
        for (std::size_t i = 0; i < shown.size(); ++i) {
            if (i) nm += ", ";
            nm += renderArg(shown[i]);
        }
        nm += ">";
    }
    return prefix + nm;
}

// ---- record / enum types and environments --------------------------------------------------------------------

const Env* Program::Impl::nestedEnv(const Decl* d, const Env* outer) {
    if (!outer) return nullptr;
    auto key = std::make_pair(d, outer);
    auto it = nestedEnvs.find(key);
    if (it != nestedEnvs.end()) return it->second;
    envs.emplace_back();
    Env* e = &envs.back();
    e->outer = outer;
    e->decl = d;
    nestedEnvs.emplace(key, e);
    return e;
}

TypeId Program::Impl::declType(const Decl* d, const Env* env) {
    auto key = std::make_pair<const void*, const void*>(d, env);
    auto it = declTypes.find(key);
    if (it != declTypes.end()) return it->second;
    TypeId id = newType(d->kind == DeclKind::Enum ? TypeKind::Enum : TypeKind::Record);
    declTypes.emplace(key, id);
    TypeInfo& ti = types[id];
    ti.decl = d;
    ti.env = env;
    ti.layout.recordKind = d->recordKind;
    ti.layout.scopedEnum = d->scoped;
    ti.layout.baseName = d->name != 0 ? text(d->name) : std::string();
    ti.layout.loc = SourceLoc{d->file, d->line};
    ti.layout.name = scopeDisplay(d, env);
    if (env && env->decl == d && env->self == kNoType) const_cast<Env*>(env)->self = id;
    const Decl* p = d->parent;
    if (p && p->kind == DeclKind::Record) {
        const Env* pe = env ? env->outer : nullptr;
        if (pe || (!p->isTemplate && !p->isSpecialization))
            types[id].layout.enclosing = (pe && pe->decl == p && pe->self != kNoType) ? pe->self : declType(p, pe);
    }
    return id;
}

std::string Program::Impl::argsKey(const std::vector<TemplateArg>& args) const {
    std::string k;
    for (const TemplateArg& a : args) {
        if (a.isType) {
            k += 'T';
            k += std::to_string(a.type);
        } else {
            k += 'V';
            k += std::to_string(a.value.value);
            k += '/';
            k += std::to_string(a.value.bits);
            k += a.value.isSigned ? 's' : 'u';
        }
        k += ',';
    }
    return k;
}

TemplateArg Program::Impl::convertArg(const TemplateParam& p, TemplateArg a, Scope sc) {
    if (p.isType) {
        if (!a.isType) throw SemaError("template argument for type parameter '" + text(p.name) + "' is not a type");
        return a;
    }
    if (a.isType) throw SemaError("template argument for non-type parameter '" + text(p.name) + "' is a type");
    if (!p.typeRange.empty()) {
        Sema s(*this, sc, p.typeRange);
        TypeId t = s.parseTypeId();
        if (isIntegral(t)) a.value = convertValue(a.value, t);
    }
    return a;
}

void Program::Impl::completeArgs(const Decl* tmpl, const Env* outer, std::vector<TemplateArg>& args) {
    Env tmp;
    tmp.outer = outer;
    tmp.decl = tmpl;
    Scope sc{tmpl, &tmp, true};
    const std::size_t n = tmpl->params.size();
    if (args.size() > n) throw SemaError("too many template arguments for '" + text(tmpl->name) + "'");
    for (std::size_t i = 0; i < n; ++i) {
        const TemplateParam& p = tmpl->params[i];
        if (p.isPack) throw SemaError("variadic templates are not supported ('" + text(tmpl->name) + "')");
        TemplateArg a;
        if (i < args.size()) {
            a = args[i];
        } else if (p.hasDefault) {
            Sema s(*this, sc, p.defaultRange);
            if (p.isType) {
                a.isType = true;
                a.type = s.parseTypeId();
            } else {
                a.value = s.parseExprNoGt();
            }
            s.expectEnd();
        } else {
            throw SemaError("too few template arguments for '" + text(tmpl->name) + "'");
        }
        a = convertArg(p, a, sc);
        tmp.args.push_back(a);
    }
    args = std::move(tmp.args);
}

bool Program::Impl::matchSpecialization(const Decl* spec, const Env* outer, const std::vector<TemplateArg>& args,
                                        std::vector<TemplateArg>& bound) {
    const std::size_t np = spec->params.size();
    std::vector<bool> have(np, false);
    bound.assign(np, TemplateArg{});
    Env tmp;
    tmp.outer = outer;
    tmp.decl = spec;
    Scope sc{spec, &tmp, true};
    if (spec->specArgs.size() > args.size()) return false;
    for (std::size_t i = 0; i < spec->specArgs.size(); ++i) {
        const TokRange r = spec->specArgs[i];
        const TemplateArg& actual = args[i];
        // pattern: a single identifier naming a parameter of the specialization
        if (r.e == r.b + 1 && tk[r.b].kind == static_cast<std::uint8_t>(TokKind::Ident)) {
            bool matched = false;
            for (std::size_t j = 0; j < np; ++j) {
                if (spec->params[j].name != tk[r.b].sym) continue;
                const TemplateParam& tp = spec->params[j];
                if (tp.isType != actual.isType) return false;
                TemplateArg v = actual;
                if (!tp.isType && !tp.typeRange.empty()) {
                    try {
                        v = convertArg(tp, actual, sc);
                    } catch (const SemaError&) {
                        return false;
                    }
                }
                if (have[j]) {
                    if (argsKey({bound[j]}) != argsKey({v})) return false;
                } else {
                    bound[j] = v;
                    have[j] = true;
                }
                matched = true;
                break;
            }
            if (matched) continue;
        }
        // concrete pattern
        try {
            Sema s(*this, sc, r);
            TemplateArg pat;
            if (actual.isType) {
                pat.isType = true;
                pat.type = s.parseTypeId();
            } else {
                pat.value = s.parseExprNoGt();
                pat.value = normalize(pat.value.value, actual.value.bits, actual.value.isSigned);
            }
            s.expectEnd();
            if (argsKey({pat}) != argsKey({actual})) return false;
        } catch (const SemaError&) {
            return false;
        }
    }
    for (std::size_t j = 0; j < np; ++j)
        if (!have[j]) return false;
    return true;
}

TypeId Program::Impl::instantiate(const Decl* primary, const Env* outer, std::vector<TemplateArg> args) {
    // Quick path: identical raw arguments seen before.
    std::string rawKey = std::to_string(reinterpret_cast<std::uintptr_t>(primary)) + '@' +
                         std::to_string(reinterpret_cast<std::uintptr_t>(outer)) + ':' + argsKey(args);
    auto rit = rawInstances.find(rawKey);
    if (rit != rawInstances.end()) return rit->second;

    if (instDepth >= opts.maxInstantiationDepth)
        throw SemaError("template instantiation depth exceeded for '" + text(primary->name) + "'");
    DepthGuard guard(instDepth);

    completeArgs(primary, outer, args);
    std::string key = std::to_string(reinterpret_cast<std::uintptr_t>(primary)) + '@' +
                      std::to_string(reinterpret_cast<std::uintptr_t>(outer)) + ':' + argsKey(args);
    auto it = instances.find(key);
    if (it != instances.end()) {
        rawInstances.emplace(rawKey, it->second);
        return it->second;
    }

    const Decl* body = primary;
    std::vector<TemplateArg> bodyArgs = args;
    const Decl* bestSpec = nullptr;
    std::vector<TemplateArg> bestBound;
    for (const Decl* spec : primary->specs) {
        std::vector<TemplateArg> bound;
        if (!matchSpecialization(spec, outer, args, bound)) continue;
        if (!bestSpec || spec->params.size() < bestSpec->params.size()) {
            bestSpec = spec;
            bestBound = std::move(bound);
        }
    }
    if (bestSpec) {
        body = bestSpec;
        bodyArgs = std::move(bestBound);
    }

    envs.emplace_back();
    Env* env = &envs.back();
    env->outer = outer;
    env->decl = body;
    env->args = std::move(bodyArgs);
    if (bestSpec) env->primaryArgs = args;

    TypeId id = newType(TypeKind::Record);
    instances.emplace(key, id);
    rawInstances.emplace(rawKey, id);
    env->self = id;
    TypeInfo& ti = types[id];
    ti.decl = body;
    ti.env = env;
    ti.layout.recordKind = body->recordKind;
    ti.layout.baseName = text(body->name);
    ti.layout.loc = SourceLoc{body->file, body->line};
    ti.layout.name = scopeDisplay(body, env);
    ti.layout.templateName = qualifiedName(primary);
    for (const TemplateArg& a : args) {
        TemplateArgInfo ai;
        ai.isType = a.isType;
        ai.type = a.type;
        ai.value = a.value.value;
        ai.text = renderArg(a);
        ti.layout.templateArgs.push_back(std::move(ai));
    }
    const Decl* p = body->parent;
    if (p && p->kind == DeclKind::Record) {
        const Env* pe = outer;
        if (pe || (!p->isTemplate && !p->isSpecialization))
            types[id].layout.enclosing = (pe && pe->decl == p && pe->self != kNoType) ? pe->self : declType(p, pe);
    }
    return id;
}

TypeId Program::Impl::instantiateAlias(const Decl* alias, const Env* outer, std::vector<TemplateArg> args) {
    if (instDepth >= opts.maxInstantiationDepth) throw SemaError("alias template instantiation depth exceeded");
    DepthGuard guard(instDepth);
    completeArgs(alias, outer, args);
    std::string key = "A" + std::to_string(reinterpret_cast<std::uintptr_t>(alias)) + '@' +
                      std::to_string(reinterpret_cast<std::uintptr_t>(outer)) + ':' + argsKey(args);
    auto it = instances.find(key);
    if (it != instances.end()) return it->second;
    envs.emplace_back();
    Env* env = &envs.back();
    env->outer = outer;
    env->decl = alias;
    env->args = args;
    TypeId t = resolveTypeSpec(alias->type, Scope{alias, env});
    instances.emplace(key, t);
    return t;
}

// ---- lookup --------------------------------------------------------------------------------------------------

Entity Program::Impl::lookupInNamespace(const Decl* ns, Sym name, std::uint8_t mask, int depth) {
    auto it = ns->byName.find(name);
    if (it != ns->byName.end()) {
        for (auto r = it->second.rbegin(); r != it->second.rend(); ++r) {
            const Decl* c = *r;
            if (!declMatches(c, mask)) continue;
            // prefer a definition over a forward declaration
            const Decl* pick = c;
            if (c->kind == DeclKind::Record && !c->defined) {
                for (const Decl* o : it->second)
                    if (o != c && o->kind == DeclKind::Record && o->defined && o->isTemplate == c->isTemplate && !o->isSpecialization) pick = o;
            }
            Entity e;
            e.kind = Entity::Kind::Decl;
            e.decl = pick;
            e.env = (pick->parent == ns) ? nullptr : nullptr;
            return e;
        }
    }
    if (depth < 8) {
        for (const Decl* u : ns->usings) {
            Entity e = lookupInNamespace(u, name, mask, depth + 1);
            if (e.kind != Entity::Kind::None) return e;
        }
    }
    return Entity{};
}

const std::vector<TypeId>& Program::Impl::basesOf(TypeId t) {
    static const std::vector<TypeId> kEmpty;
    TypeInfo& ti = types[t];
    if (ti.basesState == 2) return ti.bases;
    if (ti.basesState == 1) return kEmpty;
    if (!ti.decl || ti.decl->kind != DeclKind::Record) {
        ti.basesState = 2;
        return ti.bases;
    }
    ti.basesState = 1;
    std::vector<TypeId> result;
    const Decl* d = ti.decl;
    const Env* e = ti.env;
    for (const TokRange& r : d->bases) {
        try {
            Sema s(*this, Scope{d, e}, r);
            TypeId b = s.parseTypeId();
            if (types[b].layout.kind != TypeKind::Record) {
                diagAt(Diag::Severity::Warning, "base class '" + types[b].layout.name + "' of '" + ti.layout.name + "' is not a class", d->file, d->line);
                continue;
            }
            result.push_back(b);
        } catch (const SemaError& ex) {
            diagAt(Diag::Severity::Warning, "cannot resolve base class of '" + ti.layout.name + "': " + ex.what(), d->file, d->line);
        }
    }
    TypeInfo& again = types[t];
    again.bases = std::move(result);
    again.basesState = 2;
    return again.bases;
}

Entity Program::Impl::lookupInDecl(const Decl* d, const Env* e, Sym name, std::uint8_t mask, int depth) {
    if (depth > 16) return Entity{};
    auto it = d->byName.find(name);
    if (it != d->byName.end()) {
        for (auto r = it->second.rbegin(); r != it->second.rend(); ++r) {
            const Decl* c = *r;
            if (!declMatches(c, mask)) continue;
            const Decl* pick = c;
            if (c->kind == DeclKind::Record && !c->defined) {
                for (const Decl* o : it->second)
                    if (o != c && o->kind == DeclKind::Record && o->defined && o->isTemplate == c->isTemplate && !o->isSpecialization) pick = o;
            }
            Entity ent;
            ent.kind = Entity::Kind::Decl;
            ent.decl = pick;
            ent.env = (pick->parent == d) ? e : nestedEnv(pick->parent, e);
            return ent;
        }
    }
    if (d->kind == DeclKind::Record && !d->bases.empty() && (e || (!d->isTemplate && !d->isSpecialization))) {
        TypeId self = (e && e->self != kNoType && e->decl == d) ? e->self : declType(d, e);
        const std::vector<TypeId> bases = basesOf(self); // copy: lookups may resize caches
        for (TypeId b : bases) {
            const TypeInfo& bi = types[b];
            Entity r = lookupInDecl(bi.decl, bi.env, name, mask, depth + 1);
            if (r.kind != Entity::Kind::None) return r;
        }
    }
    return Entity{};
}

Entity Program::Impl::lookupInType(TypeId t, Sym name, std::uint8_t mask) {
    const TypeInfo& ti = types[t];
    if (!ti.decl) return Entity{};
    return lookupInDecl(ti.decl, ti.env, name, mask, 0);
}

Entity Program::Impl::lookupUnqualified(Sym name, Scope sc, std::uint8_t mask) {
    const Decl* d = sc.decl;
    const Env* e = sc.env;
    bool first = true;
    while (d) {
        if (e && e->decl == d) {
            const std::size_t n = std::min(d->params.size(), e->args.size());
            for (std::size_t i = 0; i < n; ++i) {
                if (d->params[i].name != name) continue;
                Entity r;
                if (e->args[i].isType) {
                    if (!(mask & kMaskType)) continue;
                    r.kind = Entity::Kind::Type;
                    r.type = e->args[i].type;
                } else {
                    if (!(mask & kMaskValue)) continue;
                    r.kind = Entity::Kind::Value;
                    r.value = e->args[i].value;
                }
                return r;
            }
        }
        if (!(first && sc.paramsOnly)) {
            switch (d->kind) {
            case DeclKind::Namespace: {
                Entity r = lookupInNamespace(d, name, mask, 0);
                if (r.kind != Entity::Kind::None) {
                    r.env = nullptr;
                    return r;
                }
                break;
            }
            case DeclKind::Record:
            case DeclKind::Enum: {
                Entity r = lookupInDecl(d, e, name, mask, 0);
                if (r.kind != Entity::Kind::None) return r;
                if ((mask & kMaskType) && d->kind == DeclKind::Record && d->name == name) {
                    if (e && e->decl == d && e->self != kNoType) {
                        Entity s;
                        s.kind = Entity::Kind::Type;
                        s.type = e->self;
                        // "Name<args>" inside the template itself names the template: remember it
                        s.decl = d->isSpecialization ? d->primary : d;
                        s.env = e->outer;
                        return s;
                    }
                    if (!d->isTemplate && !d->isSpecialization) {
                        Entity s;
                        s.kind = Entity::Kind::Type;
                        s.type = declType(d, e);
                        return s;
                    }
                }
                break;
            }
            default:
                break;
            }
        }
        first = false;
        const Env* pe = e ? e->outer : nullptr;
        d = d->parent;
        e = pe;
    }
    if (mask & kMaskValue) {
        // <stdint.h> / <limits.h> macros: system headers are not part of the translation unit
        static const std::unordered_map<std::string, ConstValue> kStd = [] {
            std::unordered_map<std::string, ConstValue> m;
            auto add = [&](const char* n, std::int64_t v, std::uint8_t bits, bool sgn) { m.emplace(n, Program::Impl::normalize(v, bits, sgn)); };
            add("INT8_MAX", 127, 32, true); add("INT8_MIN", -128, 32, true); add("UINT8_MAX", 255, 32, true);
            add("INT16_MAX", 32767, 32, true); add("INT16_MIN", -32768, 32, true); add("UINT16_MAX", 65535, 32, true);
            add("INT32_MAX", INT32_MAX, 32, true); add("INT32_MIN", INT32_MIN, 32, true);
            add("UINT32_MAX", static_cast<std::int64_t>(UINT32_MAX), 32, false);
            add("INT64_MAX", INT64_MAX, 64, true); add("INT64_MIN", INT64_MIN, 64, true);
            add("UINT64_MAX", -1, 64, false); add("SIZE_MAX", -1, 64, false);
            add("CHAR_BIT", 8, 32, true); add("SCHAR_MAX", 127, 32, true); add("SCHAR_MIN", -128, 32, true);
            add("UCHAR_MAX", 255, 32, true); add("SHRT_MAX", 32767, 32, true); add("SHRT_MIN", -32768, 32, true);
            add("USHRT_MAX", 65535, 32, true); add("INT_MAX", INT32_MAX, 32, true); add("INT_MIN", INT32_MIN, 32, true);
            add("UINT_MAX", static_cast<std::int64_t>(UINT32_MAX), 32, false);
            add("LLONG_MAX", INT64_MAX, 64, true); add("LLONG_MIN", INT64_MIN, 64, true); add("ULLONG_MAX", -1, 64, false);
            return m;
        }();
        auto it = kStd.find(text(name));
        if (it != kStd.end()) {
            Entity r;
            r.kind = Entity::Kind::Value;
            r.value = it->second;
            return r;
        }
    }
    return Entity{};
}

// ---- lazily evaluated declarations ---------------------------------------------------------------------------

TypeId Program::Impl::resolveTypedef(const Decl* d, const Env* env) {
    auto key = std::make_pair(d, env);
    auto it = typedefCache.find(key);
    if (it != typedefCache.end()) return it->second;
    if (!inProgress.insert(key).second) throw SemaError("circular typedef '" + text(d->name) + "'");
    struct Cleanup {
        std::set<std::pair<const Decl*, const Env*>>& s;
        std::pair<const Decl*, const Env*> k;
        ~Cleanup() { s.erase(k); }
    } cleanup{inProgress, key};
    TypeId t = resolveTypeSpec(d->type, Scope{d->parent, env});
    typedefCache.emplace(key, t);
    return t;
}

std::uint64_t Program::Impl::countInitElements(TokRange init, TypeId elem) {
    if (init.empty()) return 0;
    const Tk& first = tk[init.b];
    if (first.kind == static_cast<std::uint8_t>(TokKind::StringLit)) {
        if (types[elem].layout.kind != TypeKind::Char) return 0;
        std::string s;
        for (std::uint32_t i = init.b; i < init.e && tk[i].kind == static_cast<std::uint8_t>(TokKind::StringLit); ++i)
            s += decodeStringLiteral(i);
        return s.size() + 1;
    }
    if (!(first.kind == static_cast<std::uint8_t>(TokKind::Punct) && first.sym == pLBrace)) return 0;
    std::uint64_t count = 0;
    std::uint32_t q = init.b + 1;
    const std::uint32_t close = skipBalanced(init.b, init.e);
    const std::uint32_t last = close > init.b ? close - 1 : close; // the '}'
    bool inElement = false;
    while (q < last) {
        const Tk& t = tk[q];
        if (t.kind == static_cast<std::uint8_t>(TokKind::Punct)) {
            if (t.sym == pComma) {
                inElement = false;
                ++q;
                continue;
            }
            if (t.sym == pLParen || t.sym == pLBracket || t.sym == pLBrace) {
                if (!inElement) { ++count; inElement = true; }
                q = skipBalanced(q, last);
                continue;
            }
            if (t.sym == pLt && angleOpens(q)) {
                std::uint32_t r = skipAngle(q, last);
                if (r != UINT32_MAX) {
                    q = r;
                    continue;
                }
            }
        }
        if (!inElement) { ++count; inElement = true; }
        ++q;
    }
    return count;
}

TypeId Program::Impl::resolveTypeSpec(const TypeSpec& spec, Scope sc, const Decl* owner) {
    TypeId base = kNoType;
    const bool pointerLike = spec.ptrDepth > 0 || spec.isRef || spec.isFuncPtr;
    if (spec.isFuncPtr) {
        base = tVoid;
    } else if (spec.inlineDecl) {
        const Decl* idecl = spec.inlineDecl;
        base = declType(idecl, nestedEnv(idecl, sc.env));
    } else if (spec.isAuto) {
        throw SemaError("auto / decltype declarations are not supported");
    } else {
        try {
            Sema s(*this, sc, spec.tokens);
            base = s.parseTypeId();
            s.expectEnd();
        } catch (const SemaError&) {
            if (!pointerLike) throw;
            base = tVoid;
        }
    }
    for (int i = 0; i < spec.ptrDepth; ++i) base = pointerTo(base);
    if (spec.isRef || spec.isFuncPtr) base = pointerTo(base);
    // arrays: extents are outermost first; build from the innermost
    std::vector<std::uint64_t> counts;
    for (const TokRange& r : spec.extents) {
        Sema s(*this, sc, r);
        ConstValue v = s.parseExpr();
        s.expectEnd();
        if (v.value < 0) throw SemaError("negative array extent");
        counts.push_back(static_cast<std::uint64_t>(v.value));
    }
    if (spec.unsized) {
        std::uint64_t n = 0;
        if (owner && owner->hasInit) {
            TypeId elem = base;
            for (auto it = counts.rbegin(); it != counts.rend(); ++it) elem = arrayOf(elem, *it);
            n = countInitElements(owner->init, elem);
        }
        counts.insert(counts.begin(), n);
    }
    for (auto it = counts.rbegin(); it != counts.rend(); ++it) base = arrayOf(base, *it);
    return base;
}

TypeId Program::Impl::typeOfDecl(const Decl* d, const Env* env) {
    return resolveTypeSpec(d->type, Scope{d->parent, env}, d);
}

ConstValue Program::Impl::variableValue(const Decl* d, const Env* env) {
    auto key = std::make_pair(d, env);
    auto it = valueCache.find(key);
    if (it != valueCache.end()) return it->second;
    if (!d->hasInit) throw SemaError("variable '" + text(d->name) + "' has no initializer");
    if (!inProgress.insert(key).second) throw SemaError("circular constant '" + text(d->name) + "'");
    struct Cleanup {
        std::set<std::pair<const Decl*, const Env*>>& s;
        std::pair<const Decl*, const Env*> k;
        ~Cleanup() { s.erase(k); }
    } cleanup{inProgress, key};
    Scope sc{d->parent, env};
    TokRange r = d->init;
    // "x{5}" / "= {5}": unwrap one pair of braces
    if (!r.empty() && tk[r.b].kind == static_cast<std::uint8_t>(TokKind::Punct) && tk[r.b].sym == pLBrace && r.e >= r.b + 2) {
        r.b += 1;
        r.e -= 1;
    }
    Sema s(*this, sc, r);
    ConstValue v = s.parseExpr();
    s.expectEnd();
    if (!d->type.isAuto && d->type.ptrDepth == 0 && d->type.extents.empty() && !d->type.isRef) {
        TypeId t = resolveTypeSpec(d->type, sc);
        if (isIntegral(t)) v = convertValue(v, t);
    }
    valueCache.emplace(key, v);
    return v;
}

ConstValue Program::Impl::enumeratorValue(const Decl* d, const Env* env) {
    auto key = std::make_pair(d, env);
    auto it = valueCache.find(key);
    if (it != valueCache.end()) return it->second;
    if (!inProgress.insert(key).second) throw SemaError("circular enumerator '" + text(d->name) + "'");
    struct Cleanup {
        std::set<std::pair<const Decl*, const Env*>>& s;
        std::pair<const Decl*, const Env*> k;
        ~Cleanup() { s.erase(k); }
    } cleanup{inProgress, key};
    ConstValue v;
    if (d->hasValue) {
        Sema s(*this, Scope{d->parent, env}, d->value);
        v = s.parseExpr();
        s.expectEnd();
    } else if (d->prevEnumerator) {
        ConstValue prev = enumeratorValue(d->prevEnumerator, env);
        v = normalize(prev.value + 1, prev.bits < 32 ? 32 : prev.bits, prev.isSigned);
    } else {
        v = normalize(0, 32, true);
    }
    if (v.bits < 32) v = normalize(v.value, 32, true);
    valueCache.emplace(key, v);
    return v;
}

std::string Program::Impl::decodeStringLiteral(std::uint32_t tkIndex) const {
    std::string raw = tokenSpelling(tkIndex);
    std::size_t q = raw.find('"');
    if (q == std::string::npos) return {};
    std::string out;
    if (raw.find('R') != std::string::npos && raw.find('R') < q) {
        // raw string: R"delim( ... )delim"
        std::size_t open = raw.find('(', q);
        if (open == std::string::npos) return {};
        std::string delim = raw.substr(q + 1, open - q - 1);
        std::string close = ")" + delim + "\"";
        std::size_t end = raw.rfind(close);
        if (end == std::string::npos || end < open) return {};
        return raw.substr(open + 1, end - open - 1);
    }
    for (std::size_t i = q + 1; i < raw.size(); ++i) {
        char c = raw[i];
        if (c == '"') break;
        if (c != '\\' || i + 1 >= raw.size()) {
            out.push_back(c);
            continue;
        }
        char n = raw[++i];
        switch (n) {
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case 'r': out.push_back('\r'); break;
        case 'a': out.push_back('\a'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'v': out.push_back('\v'); break;
        case 'x': {
            unsigned v = 0;
            while (i + 1 < raw.size() && std::isxdigit(static_cast<unsigned char>(raw[i + 1]))) {
                char h = raw[++i];
                v = v * 16 + static_cast<unsigned>(std::isdigit(static_cast<unsigned char>(h)) ? h - '0' : (std::tolower(h) - 'a' + 10));
            }
            out.push_back(static_cast<char>(v));
            break;
        }
        default:
            if (n >= '0' && n <= '7') {
                unsigned v = static_cast<unsigned>(n - '0');
                for (int k = 0; k < 2 && i + 1 < raw.size() && raw[i + 1] >= '0' && raw[i + 1] <= '7'; ++k) v = v * 8 + static_cast<unsigned>(raw[++i] - '0');
                out.push_back(static_cast<char>(v));
            } else {
                out.push_back(n);
            }
            break;
        }
    }
    return out;
}

} // namespace qstate::cpp
