# Embedding of a (possibly not yet existing) file into the executable.
#
#   qstate_embed_file(<target> NAMESPACE <ns> SYMBOL <sym> INPUT <file> PLACEHOLDER <file>)
#
# Adds to <target> a generated source that defines (in namespace <ns>) the byte array <sym>_data, its size
# <sym>_size and <sym>_placeholder (true when INPUT did not exist and PLACEHOLDER was embedded instead).
# The decision is taken at BUILD time (every build): the UI build may finish after CMake configured. The
# generated .cpp is only rewritten when its content changes, so nothing is recompiled needlessly.
# Requires the executable target `qstate_embed` (created by native/gui/CMakeLists.txt).
set(_QSTATE_EMBED_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/QstateEmbedRun.cmake")

function(qstate_embed_file target)
    cmake_parse_arguments(ARG "" "NAMESPACE;SYMBOL;INPUT;PLACEHOLDER" "" ${ARGN})
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/embedded_${ARG_SYMBOL}.cpp")
    set(_tag "embed_${ARG_SYMBOL}")
    add_custom_target(${_tag}
        COMMAND ${CMAKE_COMMAND}
            -DEMBED_TOOL=$<TARGET_FILE:qstate_embed>
            -DINPUT=${ARG_INPUT} -DPLACEHOLDER=${ARG_PLACEHOLDER}
            -DOUTPUT=${_out} -DNAMESPACE=${ARG_NAMESPACE} -DSYMBOL=${ARG_SYMBOL}
            -P "${_QSTATE_EMBED_SCRIPT}"
        BYPRODUCTS "${_out}"
        DEPENDS qstate_embed "${ARG_PLACEHOLDER}"
        COMMENT "Embedding ${ARG_SYMBOL}"
        VERBATIM)
    set_source_files_properties("${_out}" PROPERTIES GENERATED TRUE)
    target_sources(${target} PRIVATE "${_out}")
    add_dependencies(${target} ${_tag})
endfunction()
