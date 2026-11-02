cmake_minimum_required(VERSION 3.20)
project(pbmysql_consumer_smoke LANGUAGES CXX)
include(CTest)
find_package(proto2mysql CONFIG REQUIRED)
option(PBMYSQL_CONSUMER_REJECT_SHARED "Exercise configuration validation" OFF)
if(PBMYSQL_CONSUMER_REJECT_SHARED)
    add_library(unsupported SHARED anchor.cpp)
    pbmysql_generate_cpp(unsupported PROTOS automatic.proto)
endif()
add_executable(helper_direct main.cpp)
pbmysql_generate_cpp(helper_direct PROTOS automatic.proto)
add_library(helper_store STATIC anchor.cpp)
pbmysql_generate_cpp(helper_store PROTOS automatic.proto)
add_executable(helper_static main.cpp)
target_link_libraries(helper_static PRIVATE helper_store)
if(MSVC)
    target_compile_options(helper_direct PRIVATE /utf-8)
    target_compile_options(helper_static PRIVATE /utf-8)
endif()
add_test(NAME helper_direct COMMAND helper_direct)
add_test(NAME helper_static COMMAND helper_static)
add_test(NAME helper_rejects_shared COMMAND ${CMAKE_COMMAND}
    "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
    "-DPACKAGE_DIR=${proto2mysql_DIR}" "-DPREFIX=${CMAKE_PREFIX_PATH}"
    "-DIGNORE_PREFIX=${CMAKE_IGNORE_PREFIX_PATH}" "-DPROTOBUF_DIR=${Protobuf_DIR}"
    "-DPROTOBUF_INCLUDE=${Protobuf_INCLUDE_DIR}" "-DPROTOBUF_LIBRARY=${Protobuf_LIBRARY}"
    "-DPROTOC=${Protobuf_PROTOC_EXECUTABLE}" "-DMYSQL_INCLUDE=${MYSQL_INCLUDE_DIR}"
    "-DMYSQL_LIBRARY=${MYSQL_LIBRARY}" "-DZLIB_INCLUDE=${ZLIB_INCLUDE_DIR}"
    "-DZLIB_LIBRARY=${ZLIB_LIBRARY}" -P ${CMAKE_CURRENT_SOURCE_DIR}/reject_shared.cmake)
