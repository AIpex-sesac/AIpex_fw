#include "grpc_server.h"
#include "grpc_client.h"
#include "power_control.h"
#include "init.h"
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

int main(int argc, char** argv) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cerr << "[main] Aipex starting...\n";

    // Set detection threshold from environment variable or default
    const char* threshold_env = std::getenv("DETECTION_THRESHOLD");
    float threshold = threshold_env ? std::atof(threshold_env) : 0.2f;
    set_detection_threshold(threshold);

    std::string target = "localhost:50051";

    const char* p = std::getenv("GRPC_PORT");
    std::string addr = "0.0.0.0:50051";
    if (p && *p) addr = std::string("0.0.0.0:") + p;

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

    GrpcServer server(addr);
    std::thread server_thread;

    if (!init_system(server, server_thread)) {
        std::cerr << "Initialization failed, exiting\n";
        return 1;
    }

    // Initialize Hailo once (in main thread or separate thread)
    std::thread detection_thread([argc, argv](){
        std::cerr << "[main] starting hailo_object_detection (init)\n";
        hailo_object_detection(argc, argv);
        std::cerr << "[main] hailo_object_detection init complete\n";
    });

    std::cerr << "[main] waiting for termination signal\n";
    while (!g_terminate.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cerr << "[main] termination requested\n";
    if (detection_thread.joinable()) detection_thread.join();
    // hailo_cleanup();
    shutdown_system(server, server_thread);
    return 0;
}