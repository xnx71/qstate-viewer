find_package(Threads REQUIRED)

add_subdirectory(third_party)

# Modules in dependency order (each one: include/qstate/<name>/, src/, tests/, CMakeLists.txt).
#
#   cpp      generic C++ front-end: lexer, preprocessor, declaration parser, constant evaluator, layout engine
#   schema   Qubic specific schema extraction on top of cpp (contract table, semantic roles)
#   support  KangarooTwelve, identities, file access, directory watching, git mirrors, settings
#   decode   decoding of state bytes against a schema: tree nodes, container adapters, tables, search
#   rpc      transport independent JSON RPC dispatcher and event bus
#   service  the RPC methods of ui/src/rpc/contract.ts implemented on top of schema / decode / support
#   gui      qstate-viewer: the desktop app (system webview)
add_subdirectory(cpp)
add_subdirectory(schema)
add_subdirectory(support)
add_subdirectory(decode)
add_subdirectory(rpc)
add_subdirectory(service)
if(QSTATE_BUILD_GUI)
    add_subdirectory(gui)
endif()
