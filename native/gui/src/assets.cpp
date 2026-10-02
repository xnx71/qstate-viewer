#include "qstate/gui/assets.h"

#include <string>

// Defined by the generated source (see qstate_embed_file in cmake/QstateEmbed.cmake and tools/embed.cpp).
namespace qstate::gui::generated {
extern const char* const index_html_chunks[];
extern const unsigned long long index_html_chunk_sizes[];
extern const unsigned long long index_html_chunk_count;
extern const unsigned long long index_html_size;
extern const bool index_html_placeholder;
} // namespace qstate::gui::generated

namespace qstate::gui {

std::string_view embeddedIndexHtml() {
    // Concatenated once (thread-safe static initialization); the pieces stay in the read-only data segment.
    static const std::string html = [] {
        std::string text;
        text.reserve(static_cast<std::size_t>(generated::index_html_size));
        for (unsigned long long i = 0; i < generated::index_html_chunk_count; ++i) {
            text.append(generated::index_html_chunks[i], static_cast<std::size_t>(generated::index_html_chunk_sizes[i]));
        }
        return text;
    }();
    return html;
}

bool embeddedIndexIsPlaceholder() {
    return generated::index_html_placeholder;
}

} // namespace qstate::gui
