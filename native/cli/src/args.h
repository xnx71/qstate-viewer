// Internal: tiny option parser: `command --name value --flag --name=value positional...`.
#pragma once

#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace qstate::cli {

struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Args {
public:
    // `valueOptions`: options that take a value; `flags`: options without value. Anything else is a UsageError.
    static Args parse(const std::vector<std::string>& args, const std::vector<std::string>& valueOptions,
                      const std::vector<std::string>& flags);

    bool has(const std::string& name) const { return values_.count(name) != 0 || flags_.count(name) != 0; }
    std::optional<std::string> get(const std::string& name) const;
    std::string require(const std::string& name) const; // UsageError when missing
    std::vector<std::string> all(const std::string& name) const;
    bool flag(const std::string& name) const { return flags_.count(name) != 0; }
    std::optional<long long> integer(const std::string& name) const;
    const std::vector<std::string>& positional() const { return positional_; }

private:
    std::map<std::string, std::vector<std::string>> values_;
    std::map<std::string, bool> flags_;
    std::vector<std::string> positional_;
};

} // namespace qstate::cli
