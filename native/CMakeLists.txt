find_package(Threads REQUIRED)

add_subdirectory(third_party)

# Modules in dependency order. A module is built when its directory has a CMakeLists.txt
# and it is selected by QSTATE_MODULES (empty = all).
#
#   cpp      generic C++ front-end: lexer, preprocessor, declaration parser, constant evaluator, layout engine
#   schema   Qubic specific schema extraction on top of cpp (contract table, semantic roles, JSON)
#   support  KangarooTwelve, identities, file access, directory watching, git helper, settings
#   decode   decoding of state bytes against a schema: tree nodes, container adapters, tables, search
#   rpc      transport independent JSON RPC dispatcher and event bus
#   service  the RPC methods of ui/src/rpc/contract.ts implemented on top of schema / decode / support
#   httpd    HTTP + SSE transport for browser based UI development
#   cli      qstate-cli: schema / verify / dump / serve
#   gui      qstate-viewer: the desktop app (system webview)
set(_qstate_all_modules cpp schema support decode rpc service httpd cli gui)

foreach(_module IN LISTS _qstate_all_modules)
    if(QSTATE_MODULES AND NOT _module IN_LIST QSTATE_MODULES)
        continue()
    endif()
    if(_module STREQUAL "gui" AND NOT QSTATE_BUILD_GUI)
        continue()
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_module}/CMakeLists.txt")
        add_subdirectory(${_module})
    endif()
endforeach()
