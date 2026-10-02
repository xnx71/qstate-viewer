# FindWebviewDeps: the platform libraries the webview/webview C++ header needs.
#
#   find_package(WebviewDeps)     -> WebviewDeps_FOUND, imported target WebviewDeps::WebviewDeps
#
# Linux: gtk+-3.0 and webkit2gtk-4.1 (4.0 as fallback) through pkg-config. On a machine without the *-dev
# packages build a local development sysroot (no root needed):
#
#     scripts/bootstrap-sysroot.sh .sysroot
#     cmake -S . -B build -DQSTATE_WEBVIEW_SYSROOT=$PWD/.sysroot
#
# QSTATE_WEBVIEW_SYSROOT (cache variable, or environment variable of the same name) makes this module put the
# sysroot's pkgconfig directories first in PKG_CONFIG_PATH. The sysroot's .pc files are rewritten by the script
# to absolute paths, so PKG_CONFIG_SYSROOT_DIR is deliberately NOT set (it would also prefix the system
# packages). When <source>/.sysroot exists it is used automatically. Without a sysroot the system packages are
# used and nothing special is needed.
# macOS: Cocoa + WebKit frameworks. Windows: the system libraries webview uses (WebView2 is loaded at run time).
include_guard(GLOBAL)

set(QSTATE_WEBVIEW_SYSROOT "" CACHE PATH "Development sysroot made by scripts/bootstrap-sysroot.sh (Linux)")

set(WebviewDeps_FOUND FALSE)
set(_wv_reason "")

if(WIN32)
    add_library(WebviewDeps::WebviewDeps INTERFACE IMPORTED)
    target_link_libraries(WebviewDeps::WebviewDeps INTERFACE ole32 shell32 shlwapi user32 version advapi32)
    set(WebviewDeps_FOUND TRUE)
elseif(APPLE)
    find_library(_wv_cocoa Cocoa)
    find_library(_wv_webkit WebKit)
    if(_wv_cocoa AND _wv_webkit)
        add_library(WebviewDeps::WebviewDeps INTERFACE IMPORTED)
        target_link_libraries(WebviewDeps::WebviewDeps INTERFACE ${_wv_cocoa} ${_wv_webkit})
        set(WebviewDeps_FOUND TRUE)
    else()
        set(_wv_reason "Cocoa / WebKit frameworks not found")
    endif()
else()
    set(_wv_sysroot "${QSTATE_WEBVIEW_SYSROOT}")
    if(NOT _wv_sysroot AND DEFINED ENV{QSTATE_WEBVIEW_SYSROOT})
        set(_wv_sysroot "$ENV{QSTATE_WEBVIEW_SYSROOT}")
    endif()
    if(NOT _wv_sysroot AND EXISTS "${PROJECT_SOURCE_DIR}/.sysroot/sysroot-env.sh")
        set(_wv_sysroot "${PROJECT_SOURCE_DIR}/.sysroot")
        message(STATUS "WebviewDeps: using the development sysroot ${_wv_sysroot}")
    endif()

    if(_wv_sysroot)
        if(NOT IS_DIRECTORY "${_wv_sysroot}")
            set(_wv_reason "QSTATE_WEBVIEW_SYSROOT=${_wv_sysroot} is not a directory")
        else()
            file(GLOB _wv_pc_dirs "${_wv_sysroot}/usr/lib/*/pkgconfig")
            list(APPEND _wv_pc_dirs "${_wv_sysroot}/usr/lib/pkgconfig" "${_wv_sysroot}/usr/share/pkgconfig")
            list(JOIN _wv_pc_dirs ":" _wv_pc_path)
            if(DEFINED ENV{PKG_CONFIG_PATH} AND NOT "$ENV{PKG_CONFIG_PATH}" STREQUAL "")
                set(ENV{PKG_CONFIG_PATH} "${_wv_pc_path}:$ENV{PKG_CONFIG_PATH}")
            else()
                set(ENV{PKG_CONFIG_PATH} "${_wv_pc_path}")
            endif()
        endif()
    endif()

    if(NOT _wv_reason)
        find_package(PkgConfig QUIET)
        if(NOT PkgConfig_FOUND)
            set(_wv_reason "pkg-config is not installed")
        else()
            # pkg_check_modules caches its results: drop them so a changed sysroot is honoured.
            get_cmake_property(_wv_cache_vars CACHE_VARIABLES)
            foreach(_wv_var IN LISTS _wv_cache_vars)
                if(_wv_var MATCHES "^QSTATE_WK_")
                    unset(${_wv_var} CACHE)
                endif()
            endforeach()
            pkg_check_modules(QSTATE_WK QUIET IMPORTED_TARGET gtk+-3.0 webkit2gtk-4.1)
            if(NOT QSTATE_WK_FOUND)
                pkg_check_modules(QSTATE_WK QUIET IMPORTED_TARGET gtk+-3.0 webkit2gtk-4.0)
            endif()
            if(QSTATE_WK_FOUND)
                add_library(WebviewDeps::WebviewDeps INTERFACE IMPORTED)
                target_link_libraries(WebviewDeps::WebviewDeps INTERFACE PkgConfig::QSTATE_WK)
                set(WebviewDeps_FOUND TRUE)
            else()
                set(_wv_reason "gtk+-3.0 and webkit2gtk-4.1 development files not found")
            endif()
        endif()
    endif()
endif()

set(WebviewDeps_REASON "${_wv_reason}")
if(WebviewDeps_FOUND)
    message(STATUS "WebviewDeps: found")
elseif(NOT WebviewDeps_FIND_QUIETLY)
    message(STATUS "WebviewDeps: not found (${_wv_reason})")
endif()
