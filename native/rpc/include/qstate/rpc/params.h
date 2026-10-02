// Small helpers for handlers to read and validate `params` objects. All failures throw
// rpc::Error(Code::InvalidParams) with the offending key in the message.
#pragma once

#include "qstate/rpc/error.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>

namespace qstate::rpc {

namespace detail {

template <class T>
T convertParam(const nlohmann::json& value, const char* key) {
    try {
        if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
            if (!value.is_number_integer()) {
                throw Error(Code::InvalidParams, std::string("params.") + key + " must be an integer");
            }
            // nlohmann's get<T>() wraps around silently; range-check explicitly.
            bool inRange;
            if (value.is_number_unsigned()) {
                inRange = value.get<std::uint64_t>() <= static_cast<std::uint64_t>(std::numeric_limits<T>::max());
            } else {
                const auto v = value.get<std::int64_t>();
                if constexpr (std::is_signed_v<T>) {
                    inRange = v >= static_cast<std::int64_t>(std::numeric_limits<T>::min()) &&
                              (v <= 0 || static_cast<std::uint64_t>(v) <= static_cast<std::uint64_t>(std::numeric_limits<T>::max()));
                } else {
                    inRange = v >= 0 && static_cast<std::uint64_t>(v) <= static_cast<std::uint64_t>(std::numeric_limits<T>::max());
                }
            }
            if (!inRange) {
                throw Error(Code::InvalidParams, std::string("params.") + key + " is out of range");
            }
        } else if constexpr (std::is_same_v<T, bool>) {
            if (!value.is_boolean()) {
                throw Error(Code::InvalidParams, std::string("params.") + key + " must be a boolean");
            }
        } else if constexpr (std::is_same_v<T, std::string>) {
            if (!value.is_string()) {
                throw Error(Code::InvalidParams, std::string("params.") + key + " must be a string");
            }
        }
        return value.get<T>();
    } catch (const nlohmann::json::exception&) {
        throw Error(Code::InvalidParams, std::string("params.") + key + " has the wrong type or is out of range");
    }
}

} // namespace detail

// params[key] converted to T; throws when params is not an object, the key is missing or has the wrong type.
template <class T>
T requireParam(const nlohmann::json& params, const char* key) {
    if (!params.is_object()) {
        throw Error(Code::InvalidParams, "params must be an object");
    }
    auto it = params.find(key);
    if (it == params.end() || it->is_null()) {
        throw Error(Code::InvalidParams, std::string("params.") + key + " is required");
    }
    return detail::convertParam<T>(*it, key);
}

// params[key] converted to T, or nullopt when absent / null; throws when present with the wrong type.
template <class T>
std::optional<T> optionalParam(const nlohmann::json& params, const char* key) {
    if (params.is_null()) {
        return std::nullopt;
    }
    if (!params.is_object()) {
        throw Error(Code::InvalidParams, "params must be an object");
    }
    auto it = params.find(key);
    if (it == params.end() || it->is_null()) {
        return std::nullopt;
    }
    return detail::convertParam<T>(*it, key);
}

} // namespace qstate::rpc
