#include "commands.h"
#include "format.h"
#include "session.h"

#include <ostream>

namespace qstate::cli {

using nlohmann::json;

namespace {

std::string nodeValue(const json& n) {
    if (n.contains("value")) return valueText(n["value"]);
    if (n.contains("preview")) return n["preview"].get<std::string>();
    return "";
}

std::string kindText(const json& n) {
    std::string k = n["kind"];
    if (n.contains("container") && n["container"].contains("population")) {
        k += " " + std::to_string(n["container"]["population"].get<std::uint64_t>()) + "/" + std::to_string(n["container"]["capacity"].get<std::uint64_t>());
    }
    return k;
}

} // namespace

int cmdDump(const Args& args, std::ostream& out, std::ostream& err) {
    Session session;
    const long long contract = *args.integer("contract");
    json ws = session.openWorkspace(args);
    const std::string node = args.get("node").value_or("");
    const long long limit = args.integer("limit").value_or(50);
    const long long offset = args.integer("offset").value_or(0);
    if (limit < 1 || limit > 1000) throw UsageError("--limit must be between 1 and 1000");
    if (offset < 0) throw UsageError("--offset must not be negative");

    json info = session.call("state.node", {{"contract", contract}, {"id", node}});
    const json c = [&] {
        for (const json& x : ws["contracts"]) if (x["index"] == contract) return x;
        return json::object();
    }();
    out << "contract " << contract << " " << c.value("name", std::string()) << "  status " << c.value("status", std::string("?"));
    if (c.contains("file")) out << "  file " << c["file"]["size"] << " bytes";
    out << "\n";
    out << "node \"" << info["id"].get<std::string>() << "\"  " << info["typeName"].get<std::string>() << "  " << kindText(info) << "  offset "
        << hexOffset(info["offset"].get<std::uint64_t>()) << "  size " << info["size"] << (info["inFile"].get<bool>() ? "" : "  (not fully in file)");
    if (info.contains("value")) out << "  = " << valueText(info["value"]);
    else if (info.contains("preview")) out << "  = " << info["preview"].get<std::string>();
    out << "\n";
    if (info["childCount"].get<std::uint64_t>() == 0) return 0;

    json page = session.call("state.children", {{"contract", contract}, {"id", node}, {"offset", offset}, {"limit", limit},
                                                {"view", args.flag("raw") ? "raw" : "logical"}, {"hideEmpty", args.flag("hide-empty")}});
    TextTable table;
    table.header = {"LABEL", "KIND", "TYPE", "OFFSET", "SIZE", "VALUE", "NODE"};
    table.rightAlign = {false, false, false, true, true, false, false};
    for (const json& n : page["items"]) {
        table.rows.push_back({n["label"].get<std::string>(), kindText(n), truncate(n["typeName"].get<std::string>(), 40),
                              hexOffset(n["offset"].get<std::uint64_t>()), std::to_string(n["size"].get<std::uint64_t>()),
                              truncate(nodeValue(n), 90), n["id"].get<std::string>()});
    }
    out << "\n";
    table.print(out);
    const std::uint64_t shown = page["items"].size();
    out << "showing " << (shown == 0 ? 0 : offset + 1) << "-" << offset + shown << " of " << page["total"] << " children\n";
    (void)err;
    return 0;
}

int cmdTable(const Args& args, std::ostream& out, std::ostream& err) {
    Session session;
    const long long contract = *args.integer("contract");
    const std::string node = args.require("node");
    session.openWorkspace(args);
    const long long limit = args.integer("limit").value_or(20);
    const long long offset = args.integer("offset").value_or(0);
    if (limit < 1 || limit > 1000) throw UsageError("--limit must be between 1 and 1000");
    if (offset < 0) throw UsageError("--offset must not be negative");
    json describeParams = {{"contract", contract}, {"id", node}};
    if (auto view = args.get("view")) describeParams["view"] = *view;
    json info = session.call("table.describe", describeParams);

    json query = {{"contract", contract}, {"id", node}, {"offset", offset}, {"limit", limit}, {"view", info["view"]},
                  {"hideEmpty", args.flag("hide-empty")}};
    if (auto sort = args.get("sort")) {
        // "column" or "column:desc"
        bool desc = args.flag("desc");
        std::string column = *sort;
        if (auto colon = column.rfind(':'); colon != std::string::npos && column.substr(colon + 1) == "desc") {
            desc = true;
            column = column.substr(0, colon);
        }
        query["sort"] = json::array({json{{"column", column}, {"desc", desc}}});
    }
    json filters = json::array();
    for (const std::string& f : args.all("filter")) {
        // column:op[:value]
        const auto first = f.find(':');
        if (first == std::string::npos) throw UsageError("--filter needs column:op[:value], got '" + f + "'");
        const auto second = f.find(':', first + 1);
        json spec = {{"column", f.substr(0, first)}, {"op", f.substr(first + 1, second == std::string::npos ? std::string::npos : second - first - 1)}};
        if (second != std::string::npos) spec["value"] = f.substr(second + 1);
        filters.push_back(spec);
    }
    if (!filters.empty()) query["filters"] = filters;
    json page = session.call("table.rows", query);

    out << "table \"" << node << "\"  view " << info["view"].get<std::string>() << "  " << info["totalRows"] << " rows";
    if (page["total"] != info["totalRows"]) out << " (" << page["total"] << " match the filters)";
    out << "\n";
    TextTable table;
    table.header.push_back("#");
    table.rightAlign.push_back(true);
    for (const json& col : info["columns"]) {
        table.header.push_back(col["label"].get<std::string>() + " [" + col["id"].get<std::string>() + "]");
        table.rightAlign.push_back(false);
    }
    for (const json& row : page["rows"]) {
        std::vector<std::string> cells = {std::to_string(row["index"].get<std::uint64_t>())};
        for (const json& cell : row["cells"]) cells.push_back(truncate(valueText(cell), 70));
        table.rows.push_back(std::move(cells));
    }
    table.print(out);
    const std::uint64_t shown = page["rows"].size();
    out << "showing " << (shown == 0 ? 0 : offset + 1) << "-" << offset + shown << " of " << page["total"] << " rows (" << static_cast<long>(page["elapsedMs"].get<double>()) << " ms)\n";
    (void)err;
    return 0;
}

int cmdSearch(const Args& args, std::ostream& out, std::ostream& err) {
    Session session;
    const long long contract = *args.integer("contract");
    session.openWorkspace(args);
    json params = {{"contract", contract}, {"query", args.require("query")}};
    if (auto mode = args.get("mode")) params["mode"] = *mode;
    if (auto limit = args.integer("limit")) params["limit"] = *limit;
    json res = session.call("state.search", params);
    out << "pattern: " << res["pattern"]["mode"].get<std::string>() << " " << res["pattern"]["hex"].get<std::string>();
    if (res["pattern"].contains("note")) out << "  (" << res["pattern"]["note"].get<std::string>() << ")";
    out << "\n";
    for (const json& m : res["matches"]) {
        out << hexOffset(m["offset"].get<std::uint64_t>()) << "  len " << m["length"] << "  ";
        std::string path;
        for (const json& step : m["location"]["path"]) {
            const std::string label = step["label"];
            if (!path.empty()) path += " / ";
            path += label;
        }
        out << path << "  [node \"" << m["location"]["id"].get<std::string>() << "\" " << m["location"]["typeName"].get<std::string>() << "]\n";
    }
    out << res["matches"].size() << " matches" << (res["truncated"].get<bool>() ? " (truncated)" : "") << ", " << static_cast<long>(res["elapsedMs"].get<double>()) << " ms\n";
    (void)err;
    return 0;
}

int cmdDigest(const Args& args, std::ostream& out, std::ostream& err) {
    Session session;
    const long long contract = *args.integer("contract");
    session.openWorkspace(args);
    json res = session.call("state.digest", {{"contract", contract}});
    out << "contract " << contract << "  k12 " << res["k12"].get<std::string>() << "  (" << static_cast<long>(res["elapsedMs"].get<double>()) << " ms)\n";
    (void)err;
    return 0;
}

} // namespace qstate::cli
