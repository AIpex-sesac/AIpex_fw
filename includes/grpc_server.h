#pragma once
#include <string>
#include <memory>
#include <grpcpp/grpcpp.h>
#include <future>
#include <thread>
#include <atomic>
#include <mutex>
#include "service_impl.h" // ComputeServiceImpl 선언

using grpc::Server;


class GrpcServer {
public:
    GrpcServer(const std::string& server_address);
    ~GrpcServer();
    void Start();
    void Start(std::promise<void>& ready);
    void Shutdown();
private:
    std::string server_address_;
    std::unique_ptr<grpc::Server> server_;
    std::unique_ptr<grpc::ServerCompletionQueue> cq_;
    std::thread cq_thread_;

    // 실제 서비스 구현을 멤버로 유지
    ComputeServiceImpl service_;

    std::atomic<bool> shutting_down_{false};
    std::atomic<bool> cq_thread_exited_{false};
    std::mutex shutdown_mutex_;
};