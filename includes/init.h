#pragma once
#include "grpc_server.h"
#include "hailo.h"
#include <thread>

bool init_system(GrpcServer &server, std::thread &server_thread, HailoDevice &hailo);
void shutdown_system(GrpcServer &server, std::thread &server_thread, HailoDevice &hailo);