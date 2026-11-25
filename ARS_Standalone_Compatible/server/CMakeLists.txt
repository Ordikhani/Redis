cmake_minimum_required(VERSION 3.16)
project(ARS_Redis_Service VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
option(ARS_LAB_PROFILE "Use isolated local-lab database and ports (never for production)" OFF)

find_package(Qt6 6.7 REQUIRED COMPONENTS Core Network Sql)

# Production server only. Tests, demos, clients and obsolete SQLite library
# are deliberately not included in this distribution.
add_library(ars_model STATIC
    src/Core/ArsDataRecord.cpp
    include/ARS/ArsDataRecord.h
    include/ARS/ArsGlobal.h)
target_include_directories(ars_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(ars_model PUBLIC ARS_STATIC)
target_link_libraries(ars_model PUBLIC Qt6::Core)

add_executable(ars_server
    server/main.cpp
    server/Startup.cpp server/Startup.h
    server/ServerConfig.cpp server/ServerConfig.h
    server/Server.cpp server/Server.h
    server/PostgresStore.cpp server/PostgresStore.h
    server/HotColdStore.cpp server/HotColdStore.h
    server/HotColdWorker.cpp server/HotColdWorker.h server/HotColdScripts.h
    src/Redis/ArsRedisClient.cpp src/Redis/ArsRedisClient.h)
if(ARS_LAB_PROFILE)
    target_compile_definitions(ars_server PRIVATE ARS_LAB_PROFILE=1)
endif()
target_include_directories(ars_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ars_server PRIVATE ars_model Qt6::Network Qt6::Sql)

# No runtime configuration artifacts are staged or installed.
install(TARGETS ars_server RUNTIME DESTINATION bin)
