#pragma once
#include "grpc_server.h"
#include <thread>

bool init_system(GrpcServer &server, std::thread &server_thread);
void shutdown_system(GrpcServer &server, std::thread &server_thread);