// Assets compiled into the executable.
#pragma once

#include <string_view>

namespace qstate::gui {

// ui/dist/index.html as it was when the executable was built (the single-file Vite build).
std::string_view embeddedIndexHtml();
// True when ui/dist/index.html did not exist at build time and the small placeholder page is embedded.
bool embeddedIndexIsPlaceholder();

// The built-in page of --headless-selftest.
std::string_view selftestPageHtml();

} // namespace qstate::gui
