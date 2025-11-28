#include "init.h"
#include "grpc_server.h"
#include <future>
#include <iostream>

bool init_system(GrpcServer &server, std::thread &server_thread) {
    std::promise<void> started;
    auto started_fut = started.get_future();

    // 서버를 스레드로 시작
    server_thread = std::thread([&server, &started]() {
        server.Start(started);
    });

    // 서버 시작 완료 대기
    try {
        started_fut.get(); // 예외 발생하면 실패
        std::cout << "Server started\n";
    } catch (const std::exception &e) {
        std::cerr << "Server failed to start: " << e.what() << "\n";
        if (server_thread.joinable()) server_thread.join();
        return false;
    }

    return true;
}

void shutdown_system(GrpcServer &server, std::thread &server_thread) {
    // 역순으로 정리
    // hailo.shutdown();
    server.Shutdown();
    if (server_thread.joinable()) server_thread.join();
}