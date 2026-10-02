qstate_add_library(service
    DEPS qstate::rpc qstate::schema qstate::decode qstate::support nlohmann_json::nlohmann_json Threads::Threads)
# Version string of app.info: the version of the top level CMake project.
target_compile_definitions(qstate_service PRIVATE QSTATE_VERSION_STRING="${PROJECT_VERSION}")
qstate_add_tests(service)
