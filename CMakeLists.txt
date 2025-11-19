cmake_minimum_required(VERSION 3.10)

project(AipexFW LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Cross-build helper: allow passing PROTOBUF_ROOT and GRPC_ROOT for cross libs
# usage: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-rpi5.cmake -DPROTOBUF_ROOT=/opt/rpi/sysroot/usr -DGRPC_ROOT=/opt/rpi/sysroot/usr ..
if(DEFINED PROTOBUF_ROOT)
  list(APPEND CMAKE_PREFIX_PATH "${PROTOBUF_ROOT}")
endif()
if(DEFINED GRPC_ROOT)
  list(APPEND CMAKE_PREFIX_PATH "${GRPC_ROOT}")
endif()

find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)

if(TARGET protobuf::libprotobuf)
  set(PROTOBUF_LIB_TARGET protobuf::libprotobuf)
else()
  set(PROTOBUF_LIB_TARGET ${Protobuf_LIBRARIES})
endif()

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/includes
  ${CMAKE_CURRENT_SOURCE_DIR}/generated
)

set(SRC_FILES
  src/main.cpp
  src/grpc_server.cpp
  src/grpc_client.cpp
  src/hailo.cpp
  src/power_control.cpp
  src/init.cpp
  src/service_impl.cpp
  generated/ComputeService.pb.cc
  generated/ComputeService.grpc.pb.cc
  generated/data_types.pb.cc
)

add_executable(AipexFW ${SRC_FILES})

target_link_libraries(AipexFW
  PRIVATE
    gRPC::grpc++
    ${PROTOBUF_LIB_TARGET}
    pthread
    dl
)

install(TARGETS AipexFW DESTINATION bin)