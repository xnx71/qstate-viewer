# decode: state bytes + schema -> lazily expandable tree, container views, tables, search.
# decode depends on the abstract seams ByteSource / IdentityCodec; support_glue.h connects them to qstate::support.
qstate_add_library(decode DEPS qstate::schema qstate::support nlohmann_json::nlohmann_json)
qstate_add_tests(decode)
