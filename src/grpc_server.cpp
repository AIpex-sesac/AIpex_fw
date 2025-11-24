#include "grpc_server.h"
#include "service_impl.h"
#include <iostream>
#include <chrono>
#include <thread>

GrpcServer::GrpcServer(const std::string& server_address)
    : server_address_(server_address), server_(nullptr), cq_(nullptr)
{
    std::cerr << "[grpc] GrpcServer constructed for " << server_address_ << "\n";
}

GrpcServer::~GrpcServer() {
    Shutdown();
}

void GrpcServer::Shutdown() {
    // 한 번만 동작하도록 보호
    bool expected = false;
    if (!shutting_down_.compare_exchange_strong(expected, true)) {
        std::cerr << "[grpc] Shutdown() already in progress/finished, returning\n";
        return;
    }

    std::cerr << "[grpc] Shutdown() called\n";
    auto start = std::chrono::steady_clock::now();

    // 1) 서버가 있으면 수신 중지 요청
    if (server_) {
        std::cerr << "[grpc] calling server_->Shutdown()\n";
        server_->Shutdown();
    }

    // 2) completion queue 종료 요청
    if (cq_) {
        std::cerr << "[grpc] calling cq_->Shutdown()\n";
        cq_->Shutdown();
    }

    // 3) cq_thread 안전하게 join (timeout fallback)
    if (cq_thread_.joinable()) {
        std::cerr << "[grpc] waiting for cq_thread_ to finish (timeout 5s)\n";
        using namespace std::chrono_literals;
        auto start = std::chrono::steady_clock::now();
        while (!cq_thread_exited_) {
            std::this_thread::sleep_for(50ms);
            if (std::chrono::steady_clock::now() - start > 3s) {
                std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
                std::cerr << "[grpc] cq_thread_ did not exit in time (" << elapsed.count() << "s), detaching to avoid hang\n";
                try {
                    cq_thread_.detach();
                } catch (...) {
                    std::cerr << "[grpc] detach failed\n";
                }
                break;
            }
        }
        // if the thread signalled exit, join it
        if (cq_thread_.joinable() && cq_thread_exited_) {
            try { cq_thread_.join(); }
            catch (...) { std::cerr << "[grpc] join failed\n"; }
        }
    }

    // 4) release resources
    if (server_) { server_.reset(); std::cerr << "[grpc] server_ reset\n"; }
    if (cq_)    { cq_.reset();    std::cerr << "[grpc] cq_ reset\n"; }


    // 완료까지 오기에 걸리는 시간
    
    std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    std::cerr << "[grpc] Shutdown() took " << elapsed.count() << " seconds\n";
    std::cerr << "[grpc] Shutdown() complete\n";
}

void GrpcServer::Start() {
    std::promise<void> dummy;
    Start(dummy);
}

void GrpcServer::Start(std::promise<void>& ready) {
    grpc::ServerBuilder builder;

    // 1) 포트 바인딩 시도 및 확인
    builder.AddListeningPort(server_address_, grpc::InsecureServerCredentials());


    // 2) CQ 추가 및 서비스 등록 (멤버 service_ 사용)
    cq_ = builder.AddCompletionQueue();
    builder.RegisterService(&service_);

    // 3) 빌드/시작
    server_ = builder.BuildAndStart();
    if (!server_) {
        std::cerr << "[grpc] BuildAndStart failed on " << server_address_ << "\n";
        if (cq_) { cq_->Shutdown(); cq_.reset(); }
        try { ready.set_exception(std::make_exception_ptr(std::runtime_error("BuildAndStart failed"))); } catch(...) {}
        return;
    }

    // 4) CQ 폴링 스레드: 예외/종료 플래그 처리
    cq_thread_exited_ = false;
    shutting_down_ = false; // reset in case reused
    cq_thread_ = std::thread([this]() {
        void* tag = nullptr;
        bool ok = false;
        try {
            while (cq_ && cq_->Next(&tag, &ok)) {
                (void)tag; (void)ok;
                // 실제 async handling 없으면 no-op
            }
        } catch (const std::exception& e) {
            std::cerr << "[grpc] exception in cq thread: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "[grpc] unknown exception in cq thread\n";
        }
        cq_thread_exited_ = true;
        std::cerr << "[grpc] cq thread exiting\n";
    });

    std::cout << "Server listening on " << server_address_ << "\n";
    try { ready.set_value(); } catch(...) {}
    server_->Wait();

    // Wait 반환 시 cleanup
    Shutdown();
}