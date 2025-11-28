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
    std::string video_path = vid_env && *vid_env ? std::string(vid_env) : std::string("/home/intel/Documents/aipex/cpp_practice/DVO00016.mp4");
    cv::VideoCapture cap(video_path);
    if (!cap.isOpened()) {
        std::cerr << "[main] Failed to open video: " << video_path << "\n";
        shutdown_system(server, server_thread);
        return 1;
    }
    std::cerr << "[main] Opened video: " << video_path << "\n";
    // make preview window autosize (show incoming frame at its own size)
    cv::namedWindow("Aipex Preview", cv::WINDOW_AUTOSIZE);
    // Get video FPS and calculate frame delay
    double video_fps = cap.get(cv::CAP_PROP_FPS);
    if (video_fps <= 0) video_fps = 30.0; // fallback
    int frame_delay_ms = static_cast<int>(1000.0 / video_fps);
    std::cerr << "[main] Video FPS: " << video_fps << ", frame delay: " << frame_delay_ms << "ms\n";

    GrpcClient client(target);
    client.StartStreaming();

    std::cerr << "[main] streaming started to " << target << " — sending frames from video\n";
    cv::Mat frame;

    // main 안 전송 루프 바로 앞에 시작 시간 기록
    auto t_start = std::chrono::steady_clock::now();

    // 전송 루프 (기존)
    const int target_size = 640; // model input size
    client.SendRequest("wakeup");
    while (!g_terminate.load() && cap.read(frame)) {
        if (frame.empty()) break;
        
        // Rotate 90 degrees clockwise to correct orientation
        cv::Mat frame_rotated;
        cv::rotate(frame, frame_rotated, cv::ROTATE_90_CLOCKWISE);
        
        // Resize to 640x640 before sending (saves bandwidth and server preprocessing)
        cv::Mat frame_resized;
        cv::resize(frame_rotated, frame_resized, cv::Size(target_size, target_size));
        
        bool ok = client.SendFrame(frame_resized);
        if (!ok) break;

        // 색상 맵 및 텍스트/두께 스케일링 헬퍼 (전송 루프 바로 위에 위치)
        std::map<std::string, cv::Scalar> class_colors = {
            {"person", cv::Scalar(0, 0, 255)},   // red (BGR)
            {"car",    cv::Scalar(0, 255, 0)},   // green
            {"bike",   cv::Scalar(255, 0, 0)}    // blue
        };
        auto pick_color = [&](const std::string &label)->cv::Scalar{
            auto it = class_colors.find(label);
            if (it != class_colors.end()) return it->second;
            return cv::Scalar(0, 255, 255); // default: yellow
        };
        
        // Stable bbox drawer: map normalized coords directly to display image pixels.
        // Use thicker font and anti-aliased drawing for 가독성.
        auto draw_bbox_on = [&](cv::Mat &img, const GrpcClient::BBox &b){
            const int img_w = img.cols;
            const int img_h = img.rows;

            // Map normalized coords directly to displayed image pixels
            int fx = static_cast<int>(std::round(b.x * img_w));
            int fy = static_cast<int>(std::round(b.y * img_h));
            int fw = static_cast<int>(std::round(b.w * img_w));
            int fh = static_cast<int>(std::round(b.h * img_h));

            // clamp/ensure minimum visible size
            fx = std::clamp(fx, 0, img_w - 1);
            fy = std::clamp(fy, 0, img_h - 1);
            if (fw < 2) fw = std::max(2, img_w / 200);
            if (fh < 2) fh = std::max(2, img_h / 200);
            if (fx + fw > img_w) fw = img_w - fx;
            if (fy + fh > img_h) fh = img_h - fy;

            // visual params: scale by min dimension to keep proportions stable
            double scale = std::max(1.0, static_cast<double>(std::min(img_w, img_h)) / 640.0);
            int thickness = std::max(2, static_cast<int>(std::round(2.0 * scale)));
            double font_scale = 0.9 * scale;

            cv::Scalar color = pick_color(b.label);
            cv::rectangle(img, cv::Rect(fx, fy, fw, fh), color, thickness, cv::LINE_AA);

            std::ostringstream ss;
            if (!b.label.empty()) ss << b.label << " ";
            ss << std::fixed << std::setprecision(2) << b.score;
            std::string text = ss.str();

            int baseline = 0;
            cv::Size tsize = cv::getTextSize(text, cv::FONT_HERSHEY_COMPLEX, font_scale, std::max(1, thickness/2), &baseline);
            int tx = fx;
            int ty = std::max(0, fy - tsize.height - 6);
            if (tx + tsize.width + 6 > img_w) tx = std::max(0, img_w - tsize.width - 6);

            cv::rectangle(img, cv::Point(tx, ty), cv::Point(tx + tsize.width + 6, ty + tsize.height + 6), color, cv::FILLED, cv::LINE_AA);
            cv::putText(img, text, cv::Point(tx + 3, ty + tsize.height + 1), cv::FONT_HERSHEY_COMPLEX, font_scale, cv::Scalar(255,255,255), std::max(1, thickness/2), cv::LINE_AA);
        };

        // 수신된 디텍션을 한 번만 꺼냄 (동일 루프에서 재사용)
        auto dets = client.PopDetections();
        // debug logs intentionally suppressed for cleaner output

         // 서버가 포워딩한 프레임이 있으면 그걸 우선 표시
         cv::Mat remote_frame;
        if (client.PopRemoteFrame(remote_frame)) {
            // Show remote frame at its native size (incoming 크기). draw on a copy.
            cv::Mat disp = remote_frame.clone();
            for (const auto &det : dets) for (const auto &b : det.boxes) draw_bbox_on(disp, b);
            cv::imshow("Aipex Preview", disp);
        } else {
            // Local frame: shrink if too large for comfortable viewing while preserving aspect
            const int max_w = 1280;
            const int max_h = 720;
            cv::Mat disp;
            double sx = static_cast<double>(frame_rotated.cols) / max_w;
            double sy = static_cast<double>(frame_rotated.rows) / max_h;
            double max_scale = std::max(1.0, std::max(sx, sy));
            if (max_scale > 1.0) {
                int nw = static_cast<int>(std::round(frame_rotated.cols / max_scale));
                int nh = static_cast<int>(std::round(frame_rotated.rows / max_scale));
                cv::resize(frame_rotated, disp, cv::Size(nw, nh), 0, 0, cv::INTER_LINEAR);
            } else {
                disp = frame_rotated.clone();
            }
            for (const auto &det : dets) for (const auto &b : det.boxes) draw_bbox_on(disp, b);
            cv::imshow("Aipex Preview", disp);
        }

        int key = cv::waitKey(1);
        if (key == 'w' || key == 'W') {
            std::cerr << "[main] key 'w' pressed -> sending START_STREAMING command\n";
            client.SendRequest("start_streaming");
        }
        if (key == 27) { // ESC to break
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(frame_delay_ms));
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

    client.StopStreaming();
    shutdown_system(server, server_thread);
    return 0;
}