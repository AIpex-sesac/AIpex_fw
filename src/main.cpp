#include "grpc_server.h"
#include "grpc_client.h"
#include "hailo.h"
#include "power_control.h"
#include "init.h"
#include <iostream>
#include <string>
#include <thread>
#include <future>
#include <chrono>
#include <atomic>
#include <csignal>

static std::atomic<bool> g_terminate{false};
static void signal_handler(int) { g_terminate.store(true); }

int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    const char* p = std::getenv("GRPC_PORT");
    std::string addr = "0.0.0.0:50051";
    if (p && *p) addr = std::string("0.0.0.0:") + p;

    const char* t = std::getenv("GRPC_TARGET");
    std::string target = t && *t ? std::string(t) : std::string("127.0.0.1:50051");

    GrpcServer server(addr);
    std::thread server_thread;
    HailoDevice hailo("device_001");

    if (!init_system(server, server_thread, hailo)) {
        std::cerr << "Initialization failed, exiting\n";
        return 1;
    }

    // 테스트용 내부 client: 스트리밍 열고 주기적 heartbeat 전송
    GrpcClient client(target);
    client.StartStreaming();

    std::cerr << "[main] streaming started to " << target << " — sending heartbeat every 1s\n";
    while (!g_terminate.load()) {
        client.SendRequest("heartbeat");
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cerr << "[main] termination requested, stopping client and shutting down server\n";
    client.StopStreaming();
    shutdown_system(server, server_thread, hailo);
    return 0;
}