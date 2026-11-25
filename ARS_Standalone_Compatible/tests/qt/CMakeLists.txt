cmake_minimum_required(VERSION 3.16)
project(ARS_Reference_Features LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(QT NAMES Qt5 Qt6 REQUIRED COMPONENTS Core Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Network)
add_executable(ars_all_features_test all_features.cpp
    ../../producer/vendor/ars/client/ArsClient.cpp
    ../../producer/vendor/ars/client/ArsClient.h
    ../../producer/vendor/ars/model/ArsDataRecord.cpp)
target_include_directories(ars_all_features_test PRIVATE
    ../../producer/vendor/ars/client ../../producer/vendor/ars/include)
target_compile_definitions(ars_all_features_test PRIVATE ARS_STATIC)
target_link_libraries(ars_all_features_test PRIVATE Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Network)
