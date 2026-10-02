# Vendored header-only dependencies (see README.md for versions and licenses).

add_library(nlohmann_json INTERFACE)
add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
target_include_directories(nlohmann_json SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(doctest INTERFACE)
add_library(doctest::doctest ALIAS doctest)
target_include_directories(doctest SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(httplib INTERFACE)
add_library(httplib::httplib ALIAS httplib)
target_include_directories(httplib SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(httplib INTERFACE Threads::Threads)
if(WIN32)
    target_link_libraries(httplib INTERFACE ws2_32 crypt32)
endif()
