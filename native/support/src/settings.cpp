#include "qstate/support/settings.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "platform.h"

namespace qstate::support {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string trimSeparators(std::string s) {
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}

bool sameWorkspace(const WorkspaceRequest& a, const WorkspaceRequest& b) {
    return trimSeparators(a.core.repoUrl) == trimSeparators(b.core.repoUrl) && a.core.ref == b.core.ref &&
           trimSeparators(a.statePath) == trimSeparators(b.statePath);
}

}  // namespace

bool isValidTheme(const std::string& theme) { return theme == "dark" || theme == "light" || theme == "system"; }

bool isComplete(const WorkspaceRequest& r) { return !r.core.repoUrl.empty() && !r.core.ref.empty() && !r.statePath.empty(); }

void to_json(json& j, const WorkspaceRequest& r) {
    j = json::object();
    j["core"] = {{"repoUrl", r.core.repoUrl}, {"ref", r.core.ref}};
    j["statePath"] = r.statePath;
    if (r.epoch) j["epoch"] = *r.epoch;
    if (!r.defines.empty()) j["defines"] = r.defines;
}

void from_json(const json& j, WorkspaceRequest& r) {
    r = WorkspaceRequest{};
    if (!j.is_object()) return;
    if (auto core = j.find("core"); core != j.end() && core->is_object()) {
        if (auto it = core->find("repoUrl"); it != core->end() && it->is_string()) r.core.repoUrl = it->get<std::string>();
        if (auto it = core->find("ref"); it != core->end() && it->is_string()) r.core.ref = it->get<std::string>();
    }
    if (auto it = j.find("statePath"); it != j.end() && it->is_string()) r.statePath = it->get<std::string>();
    if (auto it = j.find("epoch"); it != j.end() && it->is_number_integer()) r.epoch = it->get<int>();
    if (auto it = j.find("defines"); it != j.end() && it->is_array()) {
        for (const auto& d : *it) {
            if (d.is_string()) r.defines.push_back(d.get<std::string>());
        }
    }
}

void to_json(json& j, const Settings& s) {
    j = json::object();
    j["theme"] = s.theme;
    j["recentWorkspaces"] = s.recentWorkspaces;
    j["ui"] = s.ui.is_object() ? s.ui : json::object();
}

void from_json(const json& j, Settings& s) {
    s = Settings{};
    if (!j.is_object()) return;
    if (auto it = j.find("theme"); it != j.end() && it->is_string() && isValidTheme(it->get<std::string>())) {
        s.theme = it->get<std::string>();
    }
    if (auto it = j.find("recentWorkspaces"); it != j.end() && it->is_array()) {
        for (const auto& e : *it) {
            WorkspaceRequest r;
            from_json(e, r);
            if (!isComplete(r)) continue;
            if (std::none_of(s.recentWorkspaces.begin(), s.recentWorkspaces.end(),
                             [&](const WorkspaceRequest& x) { return sameWorkspace(x, r); })) {
                s.recentWorkspaces.push_back(std::move(r));
            }
            if (s.recentWorkspaces.size() >= kMaxRecentWorkspaces) break;
        }
    }
    if (auto it = j.find("ui"); it != j.end() && it->is_object()) s.ui = *it;
}

void addRecentWorkspace(std::vector<WorkspaceRequest>& list, const WorkspaceRequest& request, size_t maxEntries) {
    list.erase(std::remove_if(list.begin(), list.end(), [&](const WorkspaceRequest& x) { return sameWorkspace(x, request); }),
               list.end());
    list.insert(list.begin(), request);
    if (list.size() > maxEntries) list.resize(maxEntries);
}

SettingsStore::SettingsStore(std::string path) : path_(path.empty() ? defaultPath() : std::move(path)) {}

std::string SettingsStore::defaultPath() {
    const std::string base = platform::configBaseDirectory();
    if (base.empty()) return "qstate-viewer-settings.json";
    return (fs::path(base) / "qstate-viewer" / "settings.json").string();
}

Settings SettingsStore::loadLocked() {
    error_.clear();
    Settings s;
    std::error_code ec;
    if (!fs::exists(path_, ec)) {
        cached_ = s;
        loaded_ = true;
        return s;
    }
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        error_ = "cannot read settings file '" + path_ + "'";
    } else {
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        json parsed = json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            error_ = "settings file '" + path_ + "' is corrupt; using defaults";
            std::error_code cec;
            fs::copy_file(path_, path_ + ".corrupt", fs::copy_options::overwrite_existing, cec);
        } else {
            from_json(parsed, s);
        }
    }
    cached_ = s;
    loaded_ = true;
    return s;
}

bool SettingsStore::saveLocked() {
    std::error_code ec;
    const fs::path parent = fs::path(path_).parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) {
            error_ = "cannot create '" + parent.string() + "': " + ec.message();
            return false;
        }
    }
    json j = cached_;
    j["version"] = 1;
    std::string text = j.dump(2);
    text += '\n';
    std::string err;
    if (!platform::writeFileAtomic(path_, text, &err)) {
        error_ = err;
        return false;
    }
    error_.clear();
    return true;
}

void SettingsStore::ensureLoadedLocked() {
    if (!loaded_) loadLocked();
}

Settings SettingsStore::load() {
    std::lock_guard<std::mutex> lock(mutex_);
    return loadLocked();
}

Settings SettingsStore::get() {
    std::lock_guard<std::mutex> lock(mutex_);
    ensureLoadedLocked();
    return cached_;
}

bool SettingsStore::set(const Settings& settings) {
    std::lock_guard<std::mutex> lock(mutex_);
    cached_ = settings;
    if (cached_.recentWorkspaces.size() > kMaxRecentWorkspaces) cached_.recentWorkspaces.resize(kMaxRecentWorkspaces);
    if (!isValidTheme(cached_.theme)) cached_.theme = "system";
    if (!cached_.ui.is_object()) cached_.ui = json::object();
    loaded_ = true;
    return saveLocked();
}

Settings SettingsStore::modify(const std::function<void(Settings&)>& mutate) {
    std::lock_guard<std::mutex> lock(mutex_);
    ensureLoadedLocked();
    mutate(cached_);
    if (cached_.recentWorkspaces.size() > kMaxRecentWorkspaces) cached_.recentWorkspaces.resize(kMaxRecentWorkspaces);
    if (!isValidTheme(cached_.theme)) cached_.theme = "system";
    if (!cached_.ui.is_object()) cached_.ui = json::object();
    saveLocked();
    return cached_;
}

Settings SettingsStore::applyPatch(const json& patch) {
    return modify([&](Settings& s) {
        if (!patch.is_object()) return;
        if (auto it = patch.find("theme"); it != patch.end() && it->is_string() && isValidTheme(it->get<std::string>())) {
            s.theme = it->get<std::string>();
        }
        if (auto it = patch.find("ui"); it != patch.end() && it->is_object()) {
            if (!s.ui.is_object()) s.ui = json::object();
            for (auto kv = it->begin(); kv != it->end(); ++kv) {
                if (kv.value().is_null()) {
                    s.ui.erase(kv.key());
                } else {
                    s.ui[kv.key()] = kv.value();
                }
            }
        }
    });
}

Settings SettingsStore::addRecentWorkspace(const WorkspaceRequest& request) {
    return modify([&](Settings& s) { qstate::support::addRecentWorkspace(s.recentWorkspaces, request); });
}

Settings SettingsStore::clearRecentWorkspaces() {
    return modify([](Settings& s) { s.recentWorkspaces.clear(); });
}

std::string SettingsStore::lastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

}  // namespace qstate::support
