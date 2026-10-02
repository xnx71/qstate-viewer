# Vendored third-party headers

Header-only libraries, checked in so the native build works offline. Licenses are in `licenses/`.

| Library | Version | Include | Source |
| --- | --- | --- | --- |
| nlohmann/json | 3.12.0 | `<nlohmann/json.hpp>` | https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp |
| doctest | 2.5.3 | `<doctest/doctest.h>` | https://raw.githubusercontent.com/doctest/doctest/v2.5.3/doctest/doctest.h |
| webview/webview | 0.12.0 | `<webview/webview.h>` | https://raw.githubusercontent.com/webview/webview/0.12.0/core/include/webview/webview.h |

SHA-256 of the files as downloaded:

```
aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63  include/nlohmann/json.hpp
cfd518a3ef90f67e1f3ba514df23fb3627437de1a2feeba78cf5062a40021421  include/doctest/doctest.h
b1ff6e11431d031e5f7917e5301f4bcdf3910a2763a0720abfd291994eee7f7b  webview/include/webview/webview.h
22c4c38a605ae5362bdbd795b4358f3599c7a7e58d4824c4ed55e3be192386e0  webview/LICENSE
```

To update: download the new version to the same path, update this table and the hashes.

## webview/webview

Lives in `webview/` (not `include/`): the C++ API of 0.12.0 is the single amalgamated header
`webview/include/webview/webview.h` (MIT, `webview/LICENSE`). It is header-only, but it pulls in the platform
libraries (GTK 3 + WebKitGTK 4.1 on Linux, Cocoa + WebKit on macOS, WebView2 loader on Windows), so it is NOT part
of the always-available targets above: `native/gui/CMakeLists.txt` defines `qstate::webview` (include path only) and
`cmake/FindWebviewDeps.cmake` finds the platform libraries. Include it from exactly one translation unit
(`native/gui/app/main.cpp`): the C API functions of the header are `inline`, but the whole backend is compiled into
whoever includes it. The vendored file is byte-identical to the 0.12.0 release (no local patches).
To update: download the new release file, update the table and the hashes.
