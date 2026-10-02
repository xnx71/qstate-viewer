// Method group "state": schema.types, state.node / children / bytes / locate / reveal / search / digest,
// table.describe / table.rows
#include "module.h"
#include "qstate/decode/decoder.h"
#include "qstate/decode/json.h"
#include "qstate/rpc/params.h"
#include "qstate/support/identity.h"
#include "qstate/support/k12_file.h"
#include "qstate/service/schema_json.h"
#include "workspace_manager.h"

#include <chrono>

namespace qstate::service {

namespace {

using nlohmann::json;
using rpc::Code;

constexpr std::uint64_t kMaxChildren = 1000;
constexpr std::uint64_t kDefaultChildren = 200;
constexpr std::uint64_t kMaxBytes = 65536;
constexpr std::uint64_t kMaxSearch = 5000;
constexpr std::uint64_t kDefaultSearch = 200;

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// Maps the exceptions of decode / support to rpc errors.
template <class F>
auto guarded(F&& f) -> decltype(f()) {
    try {
        return f();
    } catch (const rpc::Error&) {
        throw;
    } catch (const rpc::Cancelled&) {
        throw;
    } catch (const decode::NotFoundError& e) {
        throw rpc::Error(Code::NotFound, e.what());
    } catch (const decode::InvalidArgumentError& e) {
        throw rpc::Error(Code::InvalidParams, e.what());
    } catch (const decode::CancelledError&) {
        throw rpc::Error(Code::Internal, "cancelled");
    } catch (const support::FileError& e) {
        throw rpc::Error(Code::IoError, e.what());
    }
}

std::uint64_t u64Param(const json& params, const char* key, std::uint64_t fallback) {
    return rpc::optionalParam<std::uint64_t>(params, key).value_or(fallback);
}

std::uint64_t limitParam(const json& params, std::uint64_t fallback, std::uint64_t max) {
    const std::uint64_t limit = u64Param(params, "limit", fallback);
    if (limit < 1 || limit > max) {
        throw rpc::Error(Code::InvalidParams, "params.limit must be between 1 and " + std::to_string(max));
    }
    return limit;
}

struct Context {
    std::shared_ptr<Workspace> ws;
    Workspace::View view;
    decode::Query query;
};

Context decoderContext(const WorkspaceManager& manager, const json& params, rpc::CallContext& call) {
    Context c;
    c.ws = manager.require();
    const auto index = rpc::requireParam<std::uint32_t>(params, "contract");
    c.view = c.ws->view(index);
    c.query.generation = c.view.generation;
    c.query.cancel = call.cancelFlag();
    return c;
}

} // namespace

void registerStateMethods(ModuleContext& ctx) {
    auto manager = ctx.state->workspaces;

    ctx.add("schema.types", [manager](const json& params, rpc::CallContext&) -> json {
        auto ws = manager->require();
        const json ids = rpc::requireParam<json>(params, "typeIds");
        if (!ids.is_array()) throw rpc::Error(Code::InvalidParams, "params.typeIds must be an array");
        const schema::Schema& s = ws->schema();
        json out = json::array();
        for (const json& id : ids) {
            if (!id.is_number_integer()) throw rpc::Error(Code::InvalidParams, "params.typeIds must contain integers");
            if (id.get<std::int64_t>() < 0 || id.get<std::uint64_t>() >= s.types.size()) {
                throw rpc::Error(Code::NotFound, "unknown type id " + id.dump());
            }
            out.push_back(typeInfoJson(s.types[id.get<std::uint64_t>()]));
        }
        return out;
    });

    ctx.add("state.node", [manager](const json& params, rpc::CallContext& call) -> json {
        Context c = decoderContext(*manager, params, call);
        const std::string id = rpc::requireParam<std::string>(params, "id");
        return guarded([&]() -> json { return c.view.decoder->node(id, c.query); });
    });

    ctx.add("state.children", [manager](const json& params, rpc::CallContext& call) -> json {
        Context c = decoderContext(*manager, params, call);
        const std::string id = rpc::requireParam<std::string>(params, "id");
        decode::ChildrenRequest req;
        if (auto view = rpc::optionalParam<std::string>(params, "view")) {
            if (*view == "raw") {
                req.view = decode::ChildView::Raw;
            } else if (*view != "logical") {
                throw rpc::Error(Code::InvalidParams, "params.view must be \"logical\" or \"raw\"");
            }
        }
        req.offset = u64Param(params, "offset", 0);
        req.limit = limitParam(params, kDefaultChildren, kMaxChildren);
        req.hideEmpty = rpc::optionalParam<bool>(params, "hideEmpty").value_or(false);
        return guarded([&]() -> json { return c.view.decoder->children(id, req, c.query); });
    });

    ctx.add("state.bytes", [manager](const json& params, rpc::CallContext&) -> json {
        auto ws = manager->require();
        const auto index = rpc::requireParam<std::uint32_t>(params, "contract");
        const auto offset = rpc::requireParam<std::uint64_t>(params, "offset");
        const auto length = rpc::requireParam<std::uint64_t>(params, "length");
        if (length > kMaxBytes) {
            throw rpc::Error(Code::InvalidParams, "params.length must be at most " + std::to_string(kMaxBytes));
        }
        Workspace::View v = ws->fileView(index);
        return guarded([&]() -> json {
            std::vector<std::uint8_t> buf(static_cast<std::size_t>(length));
            const std::size_t got = v.reader->read(offset, buf.size(), buf.data());
            return {{"offset", offset}, {"length", got}, {"hex", support::toHex(buf.data(), got)}, {"fileSize", v.reader->size()}};
        });
    });

    ctx.add("state.locate", [manager](const json& params, rpc::CallContext& call) -> json {
        Context c = decoderContext(*manager, params, call);
        const auto offset = rpc::requireParam<std::uint64_t>(params, "offset");
        return guarded([&]() -> json { return c.view.decoder->locate(offset, c.query); });
    });

    ctx.add("state.reveal", [manager](const json& params, rpc::CallContext& call) -> json {
        Context c = decoderContext(*manager, params, call);
        const auto id = rpc::optionalParam<std::string>(params, "id");
        const auto offset = rpc::optionalParam<std::uint64_t>(params, "offset");
        if (id.has_value() == offset.has_value()) {
            throw rpc::Error(Code::InvalidParams, "state.reveal needs exactly one of params.id and params.offset");
        }
        const bool hideEmpty = rpc::optionalParam<bool>(params, "hideEmpty").value_or(false);
        return guarded([&]() -> json {
            const std::string target = id ? *id : c.view.decoder->locate(*offset, c.query).id;
            return c.view.decoder->reveal(target, hideEmpty, c.query);
        });
    });

    ctx.add("state.search", [manager](const json& params, rpc::CallContext& call) -> json {
        Context c = decoderContext(*manager, params, call);
        decode::SearchRequest req;
        req.query = rpc::requireParam<std::string>(params, "query");
        if (auto mode = rpc::optionalParam<std::string>(params, "mode")) {
            if (*mode != "auto" && *mode != "id" && *mode != "hex" && *mode != "int" && *mode != "text") {
                throw rpc::Error(Code::InvalidParams, "params.mode must be one of auto, id, hex, int, text");
            }
            req.mode = *mode;
        }
        req.limit = limitParam(params, kDefaultSearch, kMaxSearch);
        return guarded([&]() -> json { return c.view.decoder->search(req, c.query); });
    });

    ctx.add("state.digest", [manager](const json& params, rpc::CallContext& call) -> json {
        auto ws = manager->require();
        const auto index = rpc::requireParam<std::uint32_t>(params, "contract");
        Workspace::View v = ws->fileView(index);
        const auto t0 = std::chrono::steady_clock::now();
        return guarded([&]() -> json {
            support::K12FileOptions options;
            options.progress = [&call](std::uint64_t, std::uint64_t) { return !call.cancelled(); };
            auto digest = support::k12DigestFile(*v.reader, options);
            if (!digest) throw rpc::Error(Code::Internal, "cancelled");
            return {{"k12", support::toHex(digest->data(), digest->size())}, {"elapsedMs", msSince(t0)}};
        });
    });

    ctx.add("table.describe", [manager](const json& params, rpc::CallContext& call) -> json {
        Context c = decoderContext(*manager, params, call);
        const std::string id = rpc::requireParam<std::string>(params, "id");
        const std::string view = rpc::optionalParam<std::string>(params, "view").value_or("");
        return guarded([&]() -> json { return c.view.decoder->describeTable(id, view, c.query); });
    });

    ctx.add("table.rows", [manager](const json& params, rpc::CallContext& call) -> json {
        Context c = decoderContext(*manager, params, call);
        decode::TableRequest req;
        req.id = rpc::requireParam<std::string>(params, "id");
        req.view = rpc::optionalParam<std::string>(params, "view").value_or("");
        req.offset = rpc::requireParam<std::uint64_t>(params, "offset");
        req.limit = rpc::requireParam<std::uint64_t>(params, "limit");
        if (req.limit < 1 || req.limit > kMaxChildren) {
            throw rpc::Error(Code::InvalidParams, "params.limit must be between 1 and " + std::to_string(kMaxChildren));
        }
        req.hideEmpty = rpc::optionalParam<bool>(params, "hideEmpty").value_or(false);
        try {
            if (auto it = params.find("sort"); it != params.end() && !it->is_null()) {
                if (!it->is_array()) throw rpc::Error(Code::InvalidParams, "params.sort must be an array");
                for (const json& s : *it) req.sort.push_back(s.get<decode::SortSpec>());
            }
            if (auto it = params.find("filters"); it != params.end() && !it->is_null()) {
                if (!it->is_array()) throw rpc::Error(Code::InvalidParams, "params.filters must be an array");
                for (const json& f : *it) req.filters.push_back(f.get<decode::FilterSpec>());
            }
        } catch (const json::exception& e) {
            throw rpc::Error(Code::InvalidParams, std::string("params.sort / params.filters: ") + e.what());
        }
        return guarded([&]() -> json { return c.view.decoder->tableRows(req, c.query); });
    });
}

} // namespace qstate::service
