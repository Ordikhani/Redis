cmake_minimum_required(VERSION 3.16)
project(ARS_Producer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.7 REQUIRED COMPONENTS Core Network)
add_library(ars_client STATIC
    vendor/ars/client/ArsClient.cpp vendor/ars/client/ArsClient.h
    vendor/ars/model/ArsDataRecord.cpp
    vendor/ars/include/ARS/ArsDataRecord.h vendor/ars/include/ARS/ArsGlobal.h)
target_include_directories(ars_client PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/ars/client
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/ars/include)
target_compile_definitions(ars_client PUBLIC ARS_STATIC)
target_link_libraries(ars_client PUBLIC Qt6::Core Qt6::Network)
add_executable(ars_producer src/sender.cpp)
target_link_libraries(ars_producer PRIVATE ars_client)
