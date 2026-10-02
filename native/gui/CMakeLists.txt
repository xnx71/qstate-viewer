# qstate-viewer: the desktop app. Everything except the webview calls lives in the qstate_gui library so it
# can be tested without a display; the executable is only built when the webview development files exist.

qstate_add_library(gui DEPS qstate::service qstate::rpc qstate::httpd nlohmann_json::nlohmann_json)

# ui/dist/index.html is compiled into the library (placeholder page when the UI has not been built yet).
include(QstateEmbed)
add_executable(qstate_embed tools/embed.cpp)
qstate_set_warnings(qstate_embed)
qstate_embed_file(qstate_gui
    NAMESPACE qstate::gui::generated
    SYMBOL index_html
    INPUT "${PROJECT_SOURCE_DIR}/ui/dist/index.html"
    PLACEHOLDER "${CMAKE_CURRENT_SOURCE_DIR}/assets/placeholder_index.html")

qstate_add_tests(gui)

# Window-less backend for browser based UI development (no GTK / WebKit needed).
add_executable(qstate-devserver app/devserver.cpp)
target_link_libraries(qstate-devserver PRIVATE qstate::gui Threads::Threads)
qstate_set_warnings(qstate-devserver)

# vendored webview/webview (header only), see native/third_party/README.md
add_library(qstate_webview INTERFACE)
add_library(qstate::webview ALIAS qstate_webview)
target_include_directories(qstate_webview SYSTEM INTERFACE "${PROJECT_SOURCE_DIR}/native/third_party/webview/include")

find_package(WebviewDeps QUIET)
if(NOT WebviewDeps_FOUND)
    message(STATUS "qstate-viewer (GUI executable) SKIPPED: webview dependencies not found (${WebviewDeps_REASON}).")
    if(UNIX AND NOT APPLE)
        message(STATUS "  Install libgtk-3-dev and libwebkit2gtk-4.1-dev, or, without root, run\n"
                       "      scripts/bootstrap-sysroot.sh ${PROJECT_SOURCE_DIR}/.sysroot\n"
                       "  and configure with -DQSTATE_WEBVIEW_SYSROOT=${PROJECT_SOURCE_DIR}/.sysroot")
    endif()
    return()
endif()

add_executable(qstate-viewer app/main.cpp)
target_link_libraries(qstate-viewer PRIVATE qstate::gui qstate::webview WebviewDeps::WebviewDeps Threads::Threads)
qstate_set_warnings(qstate-viewer)
target_compile_definitions(qstate-viewer PRIVATE QSTATE_VERSION_STRING="${PROJECT_VERSION}")
install(TARGETS qstate-viewer qstate-devserver RUNTIME DESTINATION bin)

# The bridge self test needs an X server: run it under xvfb-run.
find_program(QSTATE_XVFB_RUN xvfb-run)
if(QSTATE_BUILD_TESTS AND QSTATE_XVFB_RUN)
    add_test(NAME gui_selftest
        COMMAND "${QSTATE_XVFB_RUN}" -a -s "-screen 0 1280x900x24"
                $<TARGET_FILE:qstate-viewer> --headless-selftest --selftest-timeout 60)
    # WebKitGTK in a container / on a virtual display: see docs/HOST.md ("Headless runs").
    set_tests_properties(gui_selftest PROPERTIES
        TIMEOUT 120
        ENVIRONMENT "WEBKIT_DISABLE_COMPOSITING_MODE=1;WEBKIT_DISABLE_DMABUF_RENDERER=1;LIBGL_ALWAYS_SOFTWARE=1;GDK_BACKEND=x11")
    message(STATUS "qstate-viewer: CTest 'gui_selftest' registered (xvfb-run: ${QSTATE_XVFB_RUN})")
elseif(QSTATE_BUILD_TESTS)
    message(STATUS "qstate-viewer: xvfb-run not found, the 'gui_selftest' CTest is not registered")
endif()
