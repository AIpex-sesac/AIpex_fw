cmake_minimum_required(VERSION 3.10)

project(AipexFW LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Release 빌드로 기본 설정 (디버그 심볼 제거, 최적화)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# --- Cross-build helper
if(DEFINED PROTOBUF_ROOT)
  list(APPEND CMAKE_PREFIX_PATH "${PROTOBUF_ROOT}")
endif()
if(DEFINED GRPC_ROOT)
  list(APPEND CMAKE_PREFIX_PATH "${GRPC_ROOT}")
endif()

find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(HailoRT REQUIRED)
# OpenCV 필요한 컴포넌트만 찾기
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)

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

# Try to prefer lld linker if available to reduce link memory/time
find_program(LD_LLD NAMES ld.lld)
if(LD_LLD)
  message(STATUS "Using lld linker: ${LD_LLD}")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld")
endif()

# --- Source grouping: put heavy Hailo/AI code into a shared library to avoid huge single-link peaks
set(HAILO_SOURCES
  src/hailo_manager.cpp
  src/hailo_object_detection.cpp
  src/hailo_lowlight_enhance.cpp
  src/hailo_utils.cpp
)

set(APP_SOURCES
  src/main.cpp
  src/grpc_server.cpp
  src/grpc_client.cpp
  src/power_control.cpp
  src/init.cpp
  src/service_impl.cpp
  src/app_comm_service_impl.cpp
  src/config.cpp
)

set(GENERATED_SOURCES
  generated/app_comm.pb.cc
  generated/app_comm.grpc.pb.cc
  generated/ComputeService.pb.cc
  generated/ComputeService.grpc.pb.cc
  generated/data_types.pb.cc
)

# Build Hailo core as a shared library (reduces peak memory used at final link)
add_library(AipexCore SHARED ${HAILO_SOURCES})
target_include_directories(AipexCore PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(AipexCore
  PRIVATE
    ${OpenCV_LIBS}
    HailoRT::libhailort
    ${PROTOBUF_LIB_TARGET}
    gRPC::grpc++
    Threads::Threads
)

# Ensure PIC for everything (shared library)
set_target_properties(AipexCore PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Main executable links against the shared core + generated code + app sources
add_executable(Aipex ${APP_SOURCES} ${GENERATED_SOURCES})

target_include_directories(Aipex PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(Aipex
  PRIVATE
    AipexCore
    gRPC::grpc++
    ${PROTOBUF_LIB_TARGET}
    Threads::Threads
    ${OpenCV_LIBS}
    # Do NOT link HailoRT again here (it's in AipexCore)
)

# Disable automatic stripping during development to avoid extra memory spikes in post-build
# If you need strip in CI/release, enable it there.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  message(STATUS "Release build: strip disabled in-source to avoid OOM during link. Enable in CI if needed.")
  # add_custom_command(TARGET Aipex POST_BUILD
  #   COMMAND ${CMAKE_STRIP} --strip-unneeded $<TARGET_FILE:Aipex>
  #   COMMENT "Stripping symbols from binary"
  # )
endif()

install(TARGETS Aipex DESTINATION bin)