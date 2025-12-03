#SPDX - License - Identifier : LGPL - 3.0 - or -later
#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -
#
#Copyright IBM, 2025
#Contributor : Avani Rateria < arateria @redhat>
#
#This program is free software; you can redistribute it and / or
#modify it under the terms of the GNU Lesser General Public
#License as published by the Free Software Foundation; either
#version 3 of the License, or (at your option) any later version.
#
#This program is distributed in the hope that it will be useful,
#but WITHOUT ANY WARRANTY; without even the implied warranty of
#MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the GNU
#Lesser General Public License for more details.
#
#You should have received a copy of the GNU Lesser General Public
#License along with this library; if not, write to the Free Software
#Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110 - 1301 USA
#
#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -
#Proto Cmake

include_directories(${CMAKE_BINARY_DIR})

set(PROTO_SOURCE_DIR "${CMAKE_SOURCE_DIR}/grpc_server/proto")

file(GLOB PROTO_FILES "${PROTO_SOURCE_DIR}/*.proto")

set(PROTO_OUT_DIR "${CMAKE_BINARY_DIR}/grpc_server/proto" CACHE PATH "Path to generated proto
    files")

get_target_property(GRPC_CPP_PLUGIN_LOCATION gRPC::grpc_cpp_plugin LOCATION)

# The protoc compiler replicates the directory structure of the input
# .proto file within the output directory if that structure exists
# relative to the input path. Hence the output path is set to
# CMAKE_BINARY_DIR

message(STATUS "PROTO SOURCE DIR: ${PROTO_SOURCE_DIR}")
message(STATUS "PROTO_OUT_DIR: ${PROTO_OUT_DIR}")
message(STATUS "CMAKE_CURRENT_SOURCE_DIR: ${CMAKE_CURRENT_SOURCE_DIR}")

execute_process(
    COMMAND protoc
    --proto_path=${PROTO_SOURCE_DIR} ${PROTO_FILES}
    --grpc_out=${PROTO_OUT_DIR}
    --cpp_out=${PROTO_OUT_DIR}
    --experimental_allow_proto3_optional
    --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_LOCATION}
   WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
   RESULT_VARIABLE protoc_result
   OUTPUT_VARIABLE protoc_output
  ERROR_VARIABLE protoc_error
)

message(STATUS "PROTO RESULT: ${protoc_result}")
message(STATUS "PROTO ERROR: ${protoc_error}")
message(STATUS "PROTO OUTPUT: ${protoc_output}")

if(NOT ${protoc_result} EQUAL 0)
    message(FATAL_ERROR "protoc failed: ${protoc_error}")
    set(USE_GRPC OFF)
else()
    message(STATUS "protoc ran successfully: ${protoc_output}")
endif()

set(GENERATED_SRCS)
set(GENERATED_HDRS)

foreach(PROTO ${PROTO_FILES})
    get_filename_component(NAME_WE ${PROTO} NAME_WE)

    list(APPEND GENERATED_SRCS
        ${PROTO_OUT_DIR}/${NAME_WE}.pb.cc
        ${PROTO_OUT_DIR}/${NAME_WE}.grpc.pb.cc
    )

    list(APPEND GENERATED_HDRS
        ${PROTO_OUT_DIR}/${NAME_WE}.pb.h
        ${PROTO_OUT_DIR}/${NAME_WE}.grpc.pb.h
    )
endforeach()

add_library(proto-lib OBJECT ${GENERATED_SRCS})

target_include_directories(proto-lib PUBLIC ${PROTO_OUT_DIR})
target_link_libraries(proto-lib PUBLIC protobuf::libprotobuf)

set_target_properties(proto-lib PROPERTIES COMPILE_FLAGS "-fPIC")

set_directory_properties(PROPERTIES ADDITIONAL_MAKE_CLEAN_FILES "${PROTO_OUT_DIR}")

install(TARGETS proto-lib LIBRARY DESTINATION ${LIB_INSTALL_DIR})
