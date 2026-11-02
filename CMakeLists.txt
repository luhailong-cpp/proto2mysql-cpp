cmake_minimum_required(VERSION 3.20)
project(proto2mysql_cpp VERSION 0.3.0 LANGUAGES CXX)

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(CTest)
find_package(Threads REQUIRED)
find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
    find_package(Protobuf REQUIRED)
endif()
if(NOT Protobuf_INCLUDE_DIRS)
    get_target_property(Protobuf_INCLUDE_DIRS protobuf::libprotobuf INTERFACE_INCLUDE_DIRECTORIES)
endif()
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(MySQL REQUIRED)
function(pbmysql_copy_connector target)
    if(WIN32 AND MYSQL_RUNTIME_LIBRARY)
        add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MYSQL_RUNTIME_LIBRARY}" "$<TARGET_FILE_DIR:${target}>")
    endif()
endfunction()
set(PBMYSQL_PROTO_NAMES db_base proto2mysql_option)
set(PBMYSQL_PROTO_SOURCES)
foreach(name IN LISTS PBMYSQL_PROTO_NAMES)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${name}.pb.cc" "${CMAKE_CURRENT_BINARY_DIR}/${name}.pb.h"
        COMMAND protobuf::protoc --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
            --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto --proto_path=${Protobuf_INCLUDE_DIRS}
            ${CMAKE_CURRENT_SOURCE_DIR}/proto/${name}.proto
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/proto/${name}.proto" protobuf::protoc VERBATIM)
    list(APPEND PBMYSQL_PROTO_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/${name}.pb.cc")
endforeach()

add_library(proto2mysql
    src/mysql_wrapper/mysql_client.cpp src/mysql_wrapper/message_table.cpp
    src/mysql_wrapper/database.cpp src/mysql_wrapper/database_connection.cpp
    src/mysql_wrapper/database_schema.cpp
    src/mysql_wrapper/database_queries.cpp src/mysql_wrapper/value_codec.cpp
    src/mysql_wrapper/sql_builder.cpp src/mysql_wrapper/descriptor_registry.cpp
    src/mysql_wrapper/schema_migration.cpp ${PBMYSQL_PROTO_SOURCES})
add_library(pbmysql::proto2mysql ALIAS proto2mysql)
set(PBMYSQL_PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
include(cmake/pbmysql_generate_cpp.cmake)
target_compile_features(proto2mysql PUBLIC cxx_std_${CMAKE_CXX_STANDARD})
set_target_properties(proto2mysql PROPERTIES POSITION_INDEPENDENT_CODE ON WINDOWS_EXPORT_ALL_SYMBOLS ON)
target_include_directories(proto2mysql PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}> $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(proto2mysql PUBLIC protobuf::libprotobuf MySQL::Client Threads::Threads)
if(MSVC)
    target_compile_options(proto2mysql PRIVATE /W4 /utf-8)
    target_compile_definitions(proto2mysql PUBLIC NOMINMAX)
else()
    target_compile_options(proto2mysql PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    file(GLOB PBMYSQL_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test/*_test.cpp")
    list(REMOVE_ITEM PBMYSQL_TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/test/mysql_client_test.cpp")
    file(GLOB PBMYSQL_TEST_PROTOS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test/*.proto")
    foreach(file IN LISTS PBMYSQL_TEST_PROTOS)
        get_filename_component(name "${file}" NAME_WE)
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${name}.pb.cc" "${CMAKE_CURRENT_BINARY_DIR}/${name}.pb.h"
            COMMAND protobuf::protoc --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
                --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/test --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
                --proto_path=${Protobuf_INCLUDE_DIRS} "${file}"
            DEPENDS "${file}" ${PBMYSQL_PROTO_SOURCES} protobuf::protoc VERBATIM)
        list(APPEND PBMYSQL_TEST_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/${name}.pb.cc")
    endforeach()
    if(PBMYSQL_TEST_SOURCES)
        add_executable(proto2mysql_tests ${PBMYSQL_TEST_SOURCES})
        target_link_libraries(proto2mysql_tests PRIVATE pbmysql::proto2mysql GTest::gtest_main)
        pbmysql_copy_connector(proto2mysql_tests)
        if(MSVC)
            target_compile_options(proto2mysql_tests PRIVATE /utf-8)
        endif()
        include(GoogleTest)
        gtest_discover_tests(proto2mysql_tests)
    endif()
endif()

add_executable(proto2sql tools/proto2sql.cpp)
target_link_libraries(proto2sql PRIVATE pbmysql::proto2mysql)
list(GET Protobuf_INCLUDE_DIRS 0 PBMYSQL_PROTOBUF_FIRST_INCLUDE_DIR)
target_compile_definitions(proto2sql PRIVATE
    PBMYSQL_BUNDLED_PROTO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/proto"
    PBMYSQL_PROTOBUF_INCLUDE_DIR="${PBMYSQL_PROTOBUF_FIRST_INCLUDE_DIR}")
pbmysql_copy_connector(proto2sql)
if(MINGW)
    target_link_options(proto2sql PRIVATE -municode)
endif()
if(MSVC)
    target_compile_options(proto2sql PRIVATE /utf-8)
endif()
if(BUILD_TESTING)
    add_test(NAME proto2sql_smoke COMMAND ${CMAKE_COMMAND}
        "-DPROTOC=$<TARGET_FILE:protobuf::protoc>" "-DCLI=$<TARGET_FILE:proto2sql>"
        "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
        "-DPROTOBUF_INCLUDE=${Protobuf_INCLUDE_DIRS}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/test/proto2sql_smoke.cmake)
endif()

include(GNUInstallDirs)
install(TARGETS proto2mysql proto2sql EXPORT proto2mysqlTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY src/mysql_wrapper/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/src/mysql_wrapper FILES_MATCHING PATTERN "*.h" PATTERN "*.inc" PATTERN "mysql_database_test.pb.h" EXCLUDE)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/db_base.pb.h" "${CMAKE_CURRENT_BINARY_DIR}/proto2mysql_option.pb.h" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(WIN32 AND MYSQL_RUNTIME_LIBRARY)
    install(FILES "${MYSQL_RUNTIME_LIBRARY}" DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(FILES proto/db_base.proto proto/proto2mysql_option.proto DESTINATION ${CMAKE_INSTALL_DATADIR}/proto2mysql/proto)


include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/proto2mysqlConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/proto2mysqlConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/proto2mysql")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/proto2mysqlConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(EXPORT proto2mysqlTargets NAMESPACE pbmysql:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/proto2mysql")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/proto2mysqlConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/proto2mysqlConfigVersion.cmake"
              cmake/FindMySQL.cmake cmake/pbmysql_generate_cpp.cmake
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/proto2mysql")
