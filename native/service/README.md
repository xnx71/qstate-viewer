# native/service

The RPC methods of `ui/src/rpc/contract.ts` (namespace `qstate::service`). The webview host only sees a
`rpc::Dispatcher` + `rpc::EventBus`; this module fills them:

```cpp
qstate::rpc::Dispatcher dispatcher;
qstate::rpc::EventBus events;
qstate::service::ServiceConfig config;      // settings file, cache directory, git executable, watcher timings
qstate::service::Service service(config);
service.registerAll(dispatcher, events);    // implemented methods + "not implemented yet" stubs for the rest
```

Status: complete. Every method of contract.ts is implemented (see docs/SERVICE.md for the workspace lifecycle,
version selection, threading and event semantics). `Service::registerAll` still registers a "not implemented yet" stub
for any contract method that no group provides (a safety net when contract.ts grows).

Method groups: `app_methods.cpp`, `settings_methods.cpp`, `fs_methods.cpp`, `core_methods.cpp`, `workspace_methods.cpp`,
`state_methods.cpp`; shared machinery: `core_loader.cpp`, `workspace.cpp`, `workspace_manager.cpp`.
Handlers reach the shared state (settings store, workspace manager) through `ctx.state`.

## Extension recipe: add a method group

Example: the settings methods.

1. Create `src/settings_methods.cpp` (the CMake glob picks up new files in `src/` at the next configure/build):

   ```cpp
   #include "module.h"
   #include "qstate/rpc/params.h"

   namespace qstate::service {

   void registerSettingsMethods(ModuleContext& ctx) {
       auto store = std::make_shared<SettingsStore>(/* ... */);       // shared state: capture the shared_ptr
       ctx.add("settings.get", [store](const nlohmann::json&, rpc::CallContext&) {
           return store->toJson();
       });
       ctx.add("settings.update", [store](const nlohmann::json& params, rpc::CallContext& call) {
           auto patch = rpc::requireParam<nlohmann::json>(params, "patch");  // throws invalid_params
           return store->update(patch);
       });
   }

   } // namespace qstate::service
   ```

2. Declare it in `src/module.h` (next to `registerAppMethods`).
3. Append it to `kModules` in `src/service.cpp`.
4. Add the new dependencies to `native/service/CMakeLists.txt` (`DEPS qstate::decode ...`), tests under `tests/`.

Rules for handlers

* Signature: `(const nlohmann::json& params, rpc::CallContext&) -> nlohmann::json`; the returned value is the
  `result` of the contract (field names exactly as in contract.ts, large integers as decimal strings).
* Report failures by throwing `rpc::Error(rpc::Code::..., message, data)` (codes = `RpcErrorCode`). Any other
  exception becomes `internal`: never rely on it for expected failures.
* Handlers run on worker threads, concurrently (the dispatcher's pool). Shared state needs its own synchronization. Capture `std::shared_ptr`s, never `this` of `Service` or
  the `ModuleContext` reference (it only lives during registration).
* Long running work (schema extraction, searches) should poll `ctx.cancelled()` / call `ctx.throwIfCancelled()`.
* Events: `ctx.events.emit("workspace.updated", json)` / `"contracts.changed"` from any thread (capture a pointer
  to the bus: it outlives the handlers). The webview host coalesces and pushes them to the UI.
* Parameter validation helpers: `qstate/rpc/params.h` (`requireParam<T>`, `optionalParam<T>`).
* Large results: see docs/HOST.md ("Large payloads"): page results (`offset`/`limit`) instead of returning many MB.

The test `the method list matches ui/src/rpc/contract.ts` fails when a method is added to / removed from
contract.ts without updating `Service::contractMethods()`.
