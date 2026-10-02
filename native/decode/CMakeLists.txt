# decode: state bytes + schema -> lazily expandable tree, container views, tables, search.
# decode only depends on the abstract seams ByteSource / IdentityCodec; the glue to qstate::support
# (include/qstate/decode/support_glue.h) is compiled when the support module is part of the build.
qstate_add_library(decode DEPS qstate::schema nlohmann_json::nlohmann_json)
if(TARGET qstate::support)
    target_link_libraries(qstate_decode PUBLIC qstate::support)
    target_compile_definitions(qstate_decode PUBLIC QSTATE_DECODE_HAS_SUPPORT=1)
endif()
qstate_add_tests(decode)
