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
find_package(Threads REQUIRED)
find_package(HailoRT REQUIRED) # HailoRT는 arm64 지원, amd64 미지원
find_package(OpenCV REQUIRED)

message(STATUS "Found OpenCV: " ${OpenCV_INCLUDE_DIRS})

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
  src/power_control.cpp
  src/init.cpp
  src/service_impl.cpp
  generated/ComputeService.pb.cc
  generated/ComputeService.grpc.pb.cc
  generated/data_types.pb.cc
  src/config.cpp
  src/hailo_object_detection.cpp
)

add_executable(Aipex ${SRC_FILES})

target_link_libraries(Aipex
  PRIVATE
    gRPC::grpc++
    ${PROTOBUF_LIB_TARGET}
    pthread
    dl
    ${OpenCV_LIBS}
    HailoRT::libhailort # HailoRT는 arm64 지원, amd64 미지원
)

link_libraries(stdc++fs)

install(TARGETS Aipex DESTINATION bin)