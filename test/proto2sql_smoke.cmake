set(descriptors "${BINARY}/cli-smoke.pb")
set(output "${BINARY}/cli-smoke.sql")
execute_process(COMMAND "${PROTOC}" --include_imports "--descriptor_set_out=${descriptors}"
    "--proto_path=${SOURCE}/test" "--proto_path=${SOURCE}/proto" "--proto_path=${PROTOBUF_INCLUDE}"
    "${SOURCE}/test/proto2sql_fixture.proto" RESULT_VARIABLE result ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "protoc failed: ${error}")
endif()
execute_process(COMMAND "${CLI}" --descriptor-set "${descriptors}" RESULT_VARIABLE result OUTPUT_VARIABLE sql ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "proto2sql failed: ${error}")
endif()
foreach(table cli_modern cli_nested cli_legacy)
    if(NOT sql MATCHES "CREATE TABLE IF NOT EXISTS `${table}`")
        message(FATAL_ERROR "missing table ${table}: ${sql}")
    endif()
endforeach()
if(sql MATCHES "CREATE TABLE IF NOT EXISTS `cli_fixture.Skipped`" OR sql MATCHES "CREATE TABLE IF NOT EXISTS `cli_fixture.Container`")
    message(FATAL_ERROR "unannotated message was exported")
endif()
execute_process(COMMAND "${CLI}" --descriptor-set "${descriptors}" --out "${output}" RESULT_VARIABLE result ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "file export failed: ${error}")
endif()
file(READ "${output}" written)
if(NOT written STREQUAL sql)
    message(FATAL_ERROR "file and stdout exports differ")
endif()
file(WRITE "${BINARY}/cli-invalid.pb" "invalid protobuf bytes")
execute_process(COMMAND "${CLI}" --descriptor-set "${BINARY}/cli-invalid.pb" --out "${output}" RESULT_VARIABLE result)
if(result EQUAL 0)
    message(FATAL_ERROR "invalid descriptor set accepted")
endif()
file(READ "${output}" after_invalid)
if(NOT after_invalid STREQUAL sql)
    message(FATAL_ERROR "failed generation modified the output")
endif()
execute_process(COMMAND "${CLI}" --unknown RESULT_VARIABLE result)
if(result EQUAL 0)
    message(FATAL_ERROR "unknown flag accepted")
endif()

# Source mode accepts repeatable/CSV flags, explicit compiler paths and imports.
set(source_out "${BINARY}/cli-source space")
execute_process(COMMAND "${CLI}" --protoc "${PROTOC}" -proto "${SOURCE}/test/proto2sql_fixture.proto"
    -I "${SOURCE}/proto" -I "${PROTOBUF_INCLUDE}" -out "${source_out}"
    RESULT_VARIABLE result ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "source generation failed: ${error}")
endif()
foreach(table cli_modern cli_nested cli_legacy)
    if(NOT EXISTS "${source_out}/${table}.sql")
        message(FATAL_ERROR "source mode omitted ${table}")
    endif()
endforeach()
set(source_dir "${BINARY}/proto input")
file(MAKE_DIRECTORY "${source_dir}")
file(WRITE "${source_dir}/odd' name.proto" "syntax=\"proto3\"; import \"proto2mysql_option.proto\"; message Extra { option(proto2mysql.table_name)=\"cli_extra\"; option(proto2mysql.primary_key)=\"id\"; uint64 id=1; }")
execute_process(COMMAND "${CLI}" --protoc "${PROTOC}" -proto "${SOURCE}/test/proto2sql_fixture.proto,${source_dir}/odd' name.proto"
    -proto-file "${SOURCE}/test/proto2sql_fixture.proto" -I "${SOURCE}/proto" -I "${PROTOBUF_INCLUDE}"
    -out "${source_out}" -single "combined.sql" -drop RESULT_VARIABLE result ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "CSV/repeated/alias generation failed: ${error}")
endif()
file(READ "${source_out}/combined.sql" combined)
foreach(table cli_extra cli_legacy cli_modern cli_nested)
    if(NOT combined MATCHES "DROP TABLE IF EXISTS `${table}`" OR NOT combined MATCHES "WARNING: destructive")
        message(FATAL_ERROR "drop output omitted warning/table ${table}")
    endif()
endforeach()
string(FIND "${combined}" "CREATE TABLE IF NOT EXISTS `cli_extra`" extra_index)
string(FIND "${combined}" "CREATE TABLE IF NOT EXISTS `cli_legacy`" legacy_index)
if(NOT extra_index LESS legacy_index)
    message(FATAL_ERROR "output was not sorted by physical table name")
endif()

# Imported table messages are excluded unless they were selected input roots.
file(WRITE "${source_dir}/root.proto" "syntax=\"proto3\"; import \"proto2sql_fixture.proto\"; import \"proto2mysql_option.proto\"; message Root { option(proto2mysql.table_name)=\"cli_root\"; uint64 id=1; }")
execute_process(COMMAND "${CLI}" --protoc "${PROTOC}" -proto "${source_dir}/root.proto"
    -I "${SOURCE}/test" -I "${SOURCE}/proto" -I "${PROTOBUF_INCLUDE}" -out "${source_out}" -single "root.sql"
    RESULT_VARIABLE result ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "root filtering failed: ${error}")
endif()
file(READ "${source_out}/root.sql" root_sql)
if(root_sql MATCHES "cli_modern" OR NOT root_sql MATCHES "cli_root")
    message(FATAL_ERROR "imported table leaked into selected roots")
endif()
execute_process(COMMAND "${CLI}" --protoc "${PROTOC}" -proto "${source_dir}/root.proto" --require-db
    -I "${SOURCE}/test" -I "${SOURCE}/proto" -I "${PROTOBUF_INCLUDE}" -out "${source_out}" -single "filtered.sql"
    RESULT_VARIABLE result ERROR_VARIABLE error)
if(NOT result EQUAL 0 OR EXISTS "${source_out}/filtered.sql")
    message(FATAL_ERROR "require-db filter did not skip an unmarked root: ${error}")
endif()

# Portable output-name validation precedes writes, including device names.
foreach(name "../escape.sql" "CON.sql" "foo:bar.sql" "..hidden.sql")
    execute_process(COMMAND "${CLI}" --descriptor-set "${descriptors}" -out "${source_out}" -single "${name}" RESULT_VARIABLE result)
    if(result EQUAL 0)
        message(FATAL_ERROR "unsafe filename accepted: ${name}")
    endif()
endforeach()
file(WRITE "${source_dir}/reserved.proto" "syntax=\"proto3\"; import \"proto2mysql_option.proto\"; message Reserved { option(proto2mysql.table_name)=\"CON\"; uint64 id=1; }")
execute_process(COMMAND "${CLI}" --protoc "${PROTOC}" -proto "${source_dir}/reserved.proto"
    -I "${SOURCE}/proto" -I "${PROTOBUF_INCLUDE}" -out "${source_out}" RESULT_VARIABLE result)
if(result EQUAL 0)
    message(FATAL_ERROR "reserved per-table filename accepted")
endif()

# A later target blocked by a directory must preserve every existing output.
set(atomic_out "${BINARY}/cli-atomic-failure")
file(MAKE_DIRECTORY "${atomic_out}/cli_modern.sql")
file(WRITE "${atomic_out}/cli_legacy.sql" "original bytes")
execute_process(COMMAND "${CLI}" --descriptor-set "${descriptors}" -out "${atomic_out}" RESULT_VARIABLE result)
if(result EQUAL 0)
    message(FATAL_ERROR "blocked output target accepted")
endif()
file(READ "${atomic_out}/cli_legacy.sql" original)
if(NOT original STREQUAL "original bytes")
    message(FATAL_ERROR "failed batch modified an earlier output")
endif()

# Unicode simple-fold collisions match the runtime and Go EqualFold behavior.
file(WRITE "${source_dir}/collision.proto" "syntax=\"proto3\"; import \"proto2mysql_option.proto\"; message A { option(proto2mysql.table_name)=\"K\"; uint64 id=1; } message B { option(proto2mysql.table_name)=\"K\"; uint64 id=1; }")
execute_process(COMMAND "${CLI}" --protoc "${PROTOC}" -proto "${source_dir}/collision.proto"
    -I "${SOURCE}/proto" -I "${PROTOBUF_INCLUDE}" -out "${source_out}" RESULT_VARIABLE result)
if(result EQUAL 0)
    message(FATAL_ERROR "Unicode equivalent table names accepted")
endif()

# Windows must retain UTF-16 command-line paths instead of converting through
# its ANSI code page. The same fixtures also verify UTF-8 filesystem paths on Unix.
set(unicode_dir "${BINARY}/中文目录")
file(MAKE_DIRECTORY "${unicode_dir}")
file(WRITE "${unicode_dir}/来源.proto" "syntax=\"proto3\"; import \"proto2mysql_option.proto\"; message UnicodeRow { option(proto2mysql.table_name)=\"中文表\"; option(proto2mysql.primary_key)=\"id\"; uint64 id=1; }")
execute_process(COMMAND "${CLI}" --protoc "${PROTOC}" -proto "${unicode_dir}/来源.proto"
    -I "${SOURCE}/proto" -I "${PROTOBUF_INCLUDE}" -out "${unicode_dir}/输出" -single "架构.sql"
    RESULT_VARIABLE result ERROR_VARIABLE error)
if(NOT result EQUAL 0 OR NOT EXISTS "${unicode_dir}/输出/架构.sql")
    message(FATAL_ERROR "Unicode source/output paths failed: ${error}")
endif()
execute_process(COMMAND "${PROTOC}" --include_imports "--descriptor_set_out=${unicode_dir}/描述符.pb"
    "--proto_path=${unicode_dir}" "--proto_path=${SOURCE}/proto" "--proto_path=${PROTOBUF_INCLUDE}"
    "${unicode_dir}/来源.proto" RESULT_VARIABLE result ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Unicode descriptor generation failed: ${error}")
endif()
execute_process(COMMAND "${CLI}" --descriptor-set "${unicode_dir}/描述符.pb" --out "${unicode_dir}/导出.sql"
    RESULT_VARIABLE result ERROR_VARIABLE error)
if(NOT result EQUAL 0 OR NOT EXISTS "${unicode_dir}/导出.sql")
    message(FATAL_ERROR "Unicode descriptor/output paths failed: ${error}")
endif()
file(READ "${unicode_dir}/导出.sql" unicode_sql)
if(NOT unicode_sql MATCHES "CREATE TABLE IF NOT EXISTS `中文表`")
    message(FATAL_ERROR "Unicode SQL contents were damaged")
endif()

# Relocated installed executables find bundled schemas beside their prefix,
# independent of their original source checkout and the caller's directory.
set(prefix "${BINARY}/cli-relocated-prefix")
file(MAKE_DIRECTORY "${prefix}/bin" "${prefix}/share/proto2mysql/proto")
file(COPY "${CLI}" DESTINATION "${prefix}/bin")
get_filename_component(cli_filename "${CLI}" NAME)
file(COPY "${SOURCE}/proto/proto2mysql_option.proto" "${SOURCE}/proto/db_base.proto"
    DESTINATION "${prefix}/share/proto2mysql/proto")
file(WRITE "${prefix}/share/proto2mysql/proto/installed_only.proto" "syntax=\"proto3\"; message InstalledOnly {}")
file(WRITE "${source_dir}/installed_root.proto" "syntax=\"proto3\"; import \"proto2mysql_option.proto\"; import \"installed_only.proto\"; message InstalledRoot { option(proto2mysql.table_name)=\"installed_root\"; uint64 id=1; }")
if(WIN32)
    # This fixture relocates the executable's proto resources, not its external
    # SDK DLL deployment. Preserve the original executable's sibling DLL lookup
    # for this child process only; the caller's environment remains unchanged.
    get_filename_component(cli_runtime_directory "${CLI}" DIRECTORY)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "PATH=${cli_runtime_directory};$ENV{PATH}"
        "${prefix}/bin/${cli_filename}" --protoc "${PROTOC}"
        -proto "${source_dir}/installed_root.proto" -I "${PROTOBUF_INCLUDE}"
        -out "${prefix}/output" RESULT_VARIABLE result ERROR_VARIABLE error)
else()
    execute_process(COMMAND "${prefix}/bin/${cli_filename}" --protoc "${PROTOC}"
        -proto "${source_dir}/installed_root.proto" -I "${PROTOBUF_INCLUDE}"
        -out "${prefix}/output" RESULT_VARIABLE result ERROR_VARIABLE error)
endif()
if(NOT result EQUAL 0 OR NOT EXISTS "${prefix}/output/installed_root.sql")
    message(FATAL_ERROR "relocated installed CLI failed (result=${result}): ${error}")
endif()
