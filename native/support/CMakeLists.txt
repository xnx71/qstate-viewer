# Independent support library: K12, identities, file access, directory scan / watch, git helper, settings.
qstate_add_library(support DEPS nlohmann_json::nlohmann_json Threads::Threads)

# Optional AVX2 four-way K12 leaf hashing; selected at run time (k12.cpp checks the CPU).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    if(MSVC)
        set_source_files_properties(src/k12_avx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
    else()
        set_source_files_properties(src/k12_avx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2")
    endif()
endif()

qstate_add_tests(support)
