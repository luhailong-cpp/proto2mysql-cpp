execute_process(COMMAND "${CMAKE_COMMAND}" -S "${SOURCE}" -B "${BINARY}/reject_shared"
    "-DCMAKE_PREFIX_PATH=${PREFIX}" "-DCMAKE_IGNORE_PREFIX_PATH=${IGNORE_PREFIX}"
    "-Dproto2mysql_DIR=${PACKAGE_DIR}" "-DProtobuf_DIR=${PROTOBUF_DIR}"
    "-DProtobuf_INCLUDE_DIR=${PROTOBUF_INCLUDE}" "-DProtobuf_LIBRARY=${PROTOBUF_LIBRARY}"
    "-DProtobuf_PROTOC_EXECUTABLE=${PROTOC}" "-DMYSQL_INCLUDE_DIR=${MYSQL_INCLUDE}"
    "-DMYSQL_LIBRARY=${MYSQL_LIBRARY}" "-DZLIB_INCLUDE_DIR=${ZLIB_INCLUDE}"
    "-DZLIB_LIBRARY=${ZLIB_LIBRARY}" -DPBMYSQL_CONSUMER_REJECT_SHARED=ON
    RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(status EQUAL 0 OR NOT "${out}${err}" MATCHES "requires an executable or static library target")
    message(FATAL_ERROR "shared-target validation failed: ${status}\n${out}\n${err}")
endif()
