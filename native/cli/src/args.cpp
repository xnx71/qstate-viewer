#include "args.h"

#include <algorithm>
#include <charconv>

namespace qstate::cli {

Args Args::parse(const std::vector<std::string>& args, const std::vector<std::string>& valueOptions,
                 const std::vector<std::string>& flags) {
    Args out;
    auto isValue = [&](const std::string& n) { return std::find(valueOptions.begin(), valueOptions.end(), n) != valueOptions.end(); };
    auto isFlag = [&](const std::string& n) { return std::find(flags.begin(), flags.end(), n) != flags.end(); };
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a.size() > 2 && a.compare(0, 2, "--") == 0) {
            std::string name = a.substr(2);
            std::optional<std::string> inlineValue;
            if (auto eq = name.find('='); eq != std::string::npos) {
                inlineValue = name.substr(eq + 1);
                name = name.substr(0, eq);
            }
            if (isFlag(name)) {
                if (inlineValue) throw UsageError("option --" + name + " takes no value");
                out.flags_[name] = true;
            } else if (isValue(name)) {
                if (inlineValue) {
                    out.values_[name].push_back(*inlineValue);
                } else if (i + 1 < args.size()) {
                    out.values_[name].push_back(args[++i]);
                } else {
                    throw UsageError("option --" + name + " needs a value");
                }
            } else {
                throw UsageError("unknown option --" + name);
            }
        } else {
            out.positional_.push_back(a);
        }
    }
    return out;
}

std::optional<std::string> Args::get(const std::string& name) const {
    auto it = values_.find(name);
    if (it == values_.end() || it->second.empty()) return std::nullopt;
    return it->second.back();
}

std::string Args::require(const std::string& name) const {
    auto v = get(name);
    if (!v) throw UsageError("missing required option --" + name);
    return *v;
}

std::vector<std::string> Args::all(const std::string& name) const {
    auto it = values_.find(name);
    return it == values_.end() ? std::vector<std::string>() : it->second;
}

std::optional<long long> Args::integer(const std::string& name) const {
    auto v = get(name);
    if (!v) return std::nullopt;
    long long n = 0;
    auto [ptr, ec] = std::from_chars(v->data(), v->data() + v->size(), n);
    if (ec != std::errc() || ptr != v->data() + v->size()) throw UsageError("option --" + name + " needs an integer, got '" + *v + "'");
    return n;
}

} // namespace qstate::cli
