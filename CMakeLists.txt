cmake_minimum_required(VERSION 3.20)
project(proto2mysql_cpp VERSION 0.3.0 LANGUAGES CXX)

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(CTest)
find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
    find_package(Protobuf REQUIRED)
endif()
if(NOT Protobuf_INCLUDE_DIRS)
    get_target_property(Protobuf_INCLUDE_DIRS protobuf::libprotobuf INTERFACE_INCLUDE_DIRECTORIES)
endif()
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(MYSQL QUIET IMPORTED_TARGET mysqlclient)
    if(NOT MYSQL_FOUND)
        pkg_check_modules(MYSQL QUIET IMPORTED_TARGET mariadb)
    endif()
endif()
if(TARGET PkgConfig::MYSQL)
    set(PBMYSQL_CLIENT_TARGET PkgConfig::MYSQL)
else()
    find_path(MYSQL_INCLUDE_DIR mysql.h PATH_SUFFIXES mysql mariadb)
    find_library(MYSQL_LIBRARY NAMES mysqlclient libmysql mysql mariadb libmariadb)
    if(NOT MYSQL_INCLUDE_DIR OR NOT MYSQL_LIBRARY)
        message(FATAL_ERROR "MySQL/MariaDB client development files are required. Set MYSQL_INCLUDE_DIR and MYSQL_LIBRARY on Windows.")
    endif()
    add_library(pbmysql_mysql_client UNKNOWN IMPORTED)
    set_target_properties(pbmysql_mysql_client PROPERTIES IMPORTED_LOCATION "${MYSQL_LIBRARY}" INTERFACE_INCLUDE_DIRECTORIES "${MYSQL_INCLUDE_DIR}")
    set(PBMYSQL_CLIENT_TARGET pbmysql_mysql_client)
endif()

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
    src/mysql_wrapper/database.cpp src/mysql_wrapper/value_codec.cpp
    src/mysql_wrapper/sql_builder.cpp ${PBMYSQL_PROTO_SOURCES})
add_library(pbmysql::proto2mysql ALIAS proto2mysql)
target_include_directories(proto2mysql PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}> $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(proto2mysql PUBLIC protobuf::libprotobuf ${PBMYSQL_CLIENT_TARGET})
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
        include(GoogleTest)
        gtest_discover_tests(proto2mysql_tests)
    endif()
endif()

add_executable(proto2sql tools/proto2sql.cpp)
target_link_libraries(proto2sql PRIVATE pbmysql::proto2mysql)
if(BUILD_TESTING)
    add_test(NAME proto2sql_smoke COMMAND ${CMAKE_COMMAND}
        "-DPROTOC=$<TARGET_FILE:protobuf::protoc>" "-DCLI=$<TARGET_FILE:proto2sql>"
        "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
        "-DPROTOBUF_INCLUDE=${Protobuf_INCLUDE_DIRS}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/test/proto2sql_smoke.cmake)
endif()

include(GNUInstallDirs)
install(TARGETS proto2mysql proto2sql ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY src/mysql_wrapper/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/src/mysql_wrapper FILES_MATCHING PATTERN "*.h" PATTERN "mysql_database_test.pb.h" EXCLUDE)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/db_base.pb.h" "${CMAKE_CURRENT_BINARY_DIR}/proto2mysql_option.pb.h" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES proto/db_base.proto proto/proto2mysql_option.proto DESTINATION ${CMAKE_INSTALL_DATADIR}/proto2mysql/proto)
