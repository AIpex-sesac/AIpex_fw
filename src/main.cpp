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
#include <opencv2/opencv.hpp>

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

int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    const char* p = std::getenv("GRPC_PORT");
    std::string addr = "0.0.0.0:50051";
    if (p && *p) addr = std::string("0.0.0.0:") + p;

    const char* t = std::getenv("GRPC_TARGET");
    std::string default_target = std::string("AipexFW.local:") + (p && *p ? std::string(p) : std::string("50051"));
    std::string target = t && *t ? std::string(t) : default_target;

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

    // Load video file (or use camera if VIDEO_PATH not set)
    const char* vid_env = std::getenv("VIDEO_PATH");
    std::string video_path = vid_env && *vid_env ? std::string(vid_env) : std::string("test.mp4");
    cv::VideoCapture cap(video_path);
    if (!cap.isOpened()) {
        std::cerr << "[main] Failed to open video: " << video_path << "\n";
        shutdown_system(server, server_thread);
        return 1;
    }
    std::cerr << "[main] Opened video: " << video_path << "\n";

    GrpcClient client(target);
    client.StartStreaming();

    std::cerr << "[main] streaming started to " << target << " — sending frames from video\n";
    cv::Mat frame;

    // main 안 전송 루프 바로 앞에 시작 시간 기록
    auto t_start = std::chrono::steady_clock::now();

    // 전송 루프 (기존)
    while (!g_terminate.load() && cap.read(frame)) {
        if (frame.empty()) break;
        bool ok = client.SendFrame(frame);
        if (!ok) {
            std::cerr << "[main] SendFrame failed, stopping\n";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1)); // ~10fps
    }

    // 루프 종료 후
    auto t_end = std::chrono::steady_clock::now();
    double elapsed_sec = std::chrono::duration_cast<std::chrono::duration<double>>(t_end - t_start).count();
    uint64_t sent = client.GetSentFrames();
    uint64_t recv = client.GetReceivedResults();
    double send_fps = elapsed_sec > 0.0 ? (double)sent / elapsed_sec : 0.0;
    double recv_fps = elapsed_sec > 0.0 ? (double)recv / elapsed_sec : 0.0;
    std::cerr << "[perf] elapsed=" << elapsed_sec << "s sent=" << sent << " send_fps=" << send_fps
              << " recv=" << recv << " recv_fps=" << recv_fps << "\n";

    // 기존 정리 코드 호출
    client.StopStreaming();
    shutdown_system(server, server_thread);
    return 0;
}