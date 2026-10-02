#include "commands.h"
#include "format.h"
#include "session.h"

#include <ostream>

namespace qstate::cli {

using nlohmann::json;

namespace {

void printDiagnostics(const json& workspace, std::ostream& out) {
    for (const json& d : workspace["diagnostics"]) {
        out << d["severity"].get<std::string>() << ": " << d["message"].get<std::string>();
        if (d.contains("file")) out << "  (" << d["file"].get<std::string>() << (d.contains("line") ? ":" + std::to_string(d["line"].get<int>()) : "") << ")";
        out << "\n";
    }
}

} // namespace

int cmdVerify(const Args& args, std::ostream& out, std::ostream& err) {
    Session session;
    json ws = session.openWorkspace(args);
    const json& core = ws["core"];
    out << "core:  " << core["sourceDir"].get<std::string>() << "  (";
    out << (core["ref"].get<std::string>().empty() ? std::string("working tree") : "ref " + core["ref"].get<std::string>());
    if (core.contains("version")) out << ", version " << core["version"].get<std::string>();
    if (core.contains("epoch")) out << ", epoch " << core["epoch"];
    out << "; " << core["fileCount"] << " files, " << static_cast<long>(core["parseMs"].get<double>()) << " ms)\n";
    out << "state: " << ws["state"]["dir"].get<std::string>() << "  (";
    if (ws["state"].contains("epoch")) out << "epoch " << ws["state"]["epoch"] << "; ";
    out << "available epochs:";
    for (const json& e : ws["state"]["epochsAvailable"]) out << " " << e;
    out << ")\n\n";

    TextTable table;
    table.header = {"IDX", "NAME", "STRUCT", "EXPECTED", "FILE", "STATUS"};
    table.rightAlign = {true, false, false, true, true, false};
    int ok = 0, bad = 0, missing = 0;
    std::vector<std::string> notes;
    for (const json& c : ws["contracts"]) {
        const std::string status = c["status"];
        if (!c.contains("file")) ++missing;
        else if (status == "ok") ++ok;
        else ++bad;
        table.rows.push_back({std::to_string(c["index"].get<int>()), c["name"].get<std::string>(),
                              c.value("stateTypeName", std::string("-")),
                              c.contains("expectedSize") ? std::to_string(c["expectedSize"].get<std::uint64_t>()) : "-",
                              c.contains("file") ? std::to_string(c["file"]["size"].get<std::uint64_t>()) : "-", status});
        if (status != "ok" && c.contains("statusMessage") && c.contains("file")) {
            notes.push_back(std::to_string(c["index"].get<int>()) + " " + c["name"].get<std::string>() + ": " + c["statusMessage"].get<std::string>());
        }
    }
    table.print(out);
    out << "\n";
    for (const std::string& n : notes) out << "  " << n << "\n";
    printDiagnostics(ws, out);
    out << ok << " ok, " << bad << " not ok, " << missing << " without file\n";
    (void)err;
    return bad == 0 ? 0 : 1;
}

} // namespace qstate::cli
