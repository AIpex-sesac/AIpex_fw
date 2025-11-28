#include "grpc_server.h"
#include "grpc_client.h"
#include "power_control.h"
#include "init.h"
#include "app_comm_service_impl.h"
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <string>
#include <thread>
#include <future>
#include <chrono>
#include <atomic>
#include <csignal>
#include <netdb.h>
#include <arpa/inet.h>
#include <cstdio>
#include <memory>
#include <array>
#include "hailo_manager.h"

static std::atomic<bool> g_terminate{false};
static void signal_handler(int) { g_terminate.store(true); }

// helper: try getaddrinfo for hostname (without :port). returns IP string or empty.
static std::string resolve_hostname(const std::string& host) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0) return "";

    char buf[INET6_ADDRSTRLEN] = {0};
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
        void* addr = nullptr;
        if (p->ai_family == AF_INET) {
            addr = &((sockaddr_in*)p->ai_addr)->sin_addr;
        } else if (p->ai_family == AF_INET6) {
            addr = &((sockaddr_in6*)p->ai_addr)->sin6_addr;
        }
        if (addr && inet_ntop(p->ai_family, addr, buf, sizeof(buf))) {
            std::string ip(buf);
            freeaddrinfo(res);
            return ip;
        }
    }
    freeaddrinfo(res);
    return "";
}

// fallback: call avahi-resolve -n <name>
static std::string avahi_resolve(const std::string& name) {
    std::array<char, 256> buf;
    std::string cmd = "avahi-resolve -n " + name + " 2>/dev/null";
    std::string out;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "";
    while (fgets(buf.data(), (int)buf.size(), pipe.get()) != nullptr) out += buf.data();
    // avahi-resolve returns "name<TAB>ip\n" on success
    if (out.empty()) return "";
    size_t tab = out.find('\t');
    if (tab == std::string::npos) return "";
    std::string ip = out.substr(tab + 1);
    // trim newline
    if (!ip.empty() && ip.back() == '\n') ip.pop_back();
    return ip;
}

// forward declaration from hailo_object_detection.cpp
extern void set_detection_threshold(float threshold);

extern int hailo_object_detection(int argc, char** argv);
extern int hailo_lowlight_enhance(int argc, char** argv);

int main(int argc, char** argv) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cerr << "[main] Aipex starting...\n";

    // Set detection threshold from environment variable or default
    const char* threshold_env = std::getenv("DETECTION_THRESHOLD");
    float threshold = threshold_env ? std::atof(threshold_env) : 0.5f;
    set_detection_threshold(threshold);

    std::string target = "localhost:50051";

    const char* p = std::getenv("GRPC_PORT");
    std::string addr = "[::]:50051";
    if (p && *p) addr = std::string("[::]:") + p;

    const char* t = std::getenv("GRPC_TARGET");
    // 기본: mDNS로 광고한 서비스 이름(AipexFW.local)을 사용
    std::string default_target = std::string("AipexCB.local:") + (p && *p ? std::string(p) : std::string("50051"));
    target = t && *t ? std::string(t) : default_target;

    // if target uses .local, try native getaddrinfo then avahi-resolve fallback
    auto colon = target.find(':');
    if (colon != std::string::npos) {
        std::string host = target.substr(0, colon);
        std::string port = target.substr(colon + 1);
        if (host.size() > 6 && host.rfind(".local") == host.size() - 6) {
            std::string ip = resolve_hostname(host);
            if (ip.empty()) {
                std::cerr << "[main] getaddrinfo failed for " << host << ", trying avahi-resolve\n";
                ip = avahi_resolve(host);
            } else {
                std::cerr << "[main] resolved " << host << " -> " << ip << " via getaddrinfo\n";
            }
            if (!ip.empty()) {
                target = ip + ":" + port;
                std::cerr << "[main] using target " << target << " (resolved from " << host << ")\n";
            } else {
                std::cerr << "[main] failed to resolve " << host << ", leaving target as " << target << "\n";
            }
        }
    }

    // --- Initialize Hailo once here to avoid concurrent init from multiple threads
    const char* hef_path_env = std::getenv("HEF_PATH");
    const char* hef_path = hef_path_env && *hef_path_env ? hef_path_env : "/home/pi/hailo/best.hef";
    std::cerr << "[main] Initializing Hailo once from main with HEF: " << hef_path << "\n";
    if (HailoManager::instance().init(hef_path) != 0) {
        std::cerr << "[main] HailoManager init failed, exiting\n";
        return 1;
    }

    GrpcServer server(addr);
    std::thread server_thread;

    // --- Start separate AppComm gRPC server on port 50052 (for single-shot JSON RPC)
    std::unique_ptr<grpc::Server> app_server;
    std::thread app_thread([&app_server]() {
        AppCommServiceImpl svc;
        grpc::ServerBuilder builder;
        const std::string app_addr = std::string("[::]:") + std::to_string(50052);
        builder.AddListeningPort(app_addr, grpc::InsecureServerCredentials());
        builder.RegisterService(&svc);
        app_server = builder.BuildAndStart();
        if (app_server) {
            std::cerr << "[appcomm] listening on " << app_addr << "\n";
            app_server->Wait();
            std::cerr << "[appcomm] server stopped\n";
        } else {
            std::cerr << "[appcomm] failed to start\n";
        }
    });

    if (!init_system(server, server_thread)) {
        std::cerr << "Initialization failed, exiting\n";
        // stop app server if started
        if (app_server) app_server->Shutdown();
        if (app_thread.joinable()) app_thread.join();
        return 1;
    }

    const char* lle = std::getenv("LOWLIGHT_ENHANCE");
    std::thread lle_thread;
    std::thread detection_thread;
    if (lle && std::string(lle) == "1") {
        std::cerr << "[main] LOWLIGHT_ENHANCE=1 set, starting low light enhancement service\n";
        // start lowlight thread (no re-init). keep handle and do not detach.
        lle_thread = std::thread([argc, argv]() {
            try {
                std::cerr << "[main] starting hailo_lowlight_enhance (thread)\n";
                hailo_lowlight_enhance(argc, argv); // should not re-init
                std::cerr << "[main] hailo_lowlight_enhance thread finished\n";
            } catch (const std::exception &e) {
                std::cerr << "[main] hailo_lowlight_enhance threw: " << e.what() << "\n";
            } catch (...) {
                std::cerr << "[main] hailo_lowlight_enhance threw unknown exception\n";
            }
        });
    } else {
        detection_thread = std::thread([argc, argv](){
            try {
                std::cerr << "[main] starting hailo_object_detection (thread)\n";
                hailo_object_detection(argc, argv); // should not re-init
                std::cerr << "[main] hailo_object_detection thread finished\n";
            } catch (const std::exception &e) {
                std::cerr << "[main] hailo_object_detection threw: " << e.what() << "\n";
            } catch (...) {
                std::cerr << "[main] hailo_object_detection threw unknown exception\n";
            }
        });
    }

    std::cerr << "[main] waiting for termination signal\n";
    while (!g_terminate.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cerr << "[main] termination requested\n";
    // join any started threads
    if (lle_thread.joinable()) lle_thread.join();
    if (detection_thread.joinable()) detection_thread.join();
    // stop app server gracefully
    if (app_server) {
        std::cerr << "[main] shutting down appcomm server\n";
        app_server->Shutdown();
    }
    if (app_thread.joinable()) app_thread.join();
    // hailo_cleanup();
    shutdown_system(server, server_thread);
    return 0;
}