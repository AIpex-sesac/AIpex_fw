#include "config.h"
#include <fstream>
#include <sstream>
#include <regex>
#include <iostream>
#include <chrono>
#include <unistd.h> // gethostname
#include <sys/types.h>

static void write_config_file(const std::string& path, const AppConfig& cfg) {
    std::ofstream ofs(path, std::ofstream::trunc);
    if (!ofs) return;
    ofs << "{\n";
    ofs << "  \"device_id\": \"" << cfg.device_id << "\",\n";
    ofs << "  \"threshold\": " << cfg.threshold << ",\n";
    ofs << "  \"sleep_timeout_sec\": " << cfg.sleep_timeout_sec << "\n";
    ofs << "}\n";
    ofs.close();
}

AppConfig load_config(const std::string& path) {
    AppConfig cfg;
    std::ifstream ifs(path);
    std::string content;
    if (ifs) {
        std::ostringstream ss;
        ss << ifs.rdbuf();
        content = ss.str();
        ifs.close();

        std::smatch m;
        // device_id
        if (std::regex_search(content, m, std::regex("\"device_id\"\\s*:\\s*\"([^\"]+)\""))) {
            cfg.device_id = m[1].str();
        }
        // threshold (float)
        if (std::regex_search(content, m, std::regex("\"threshold\"\\s*:\\s*([0-9]+(?:\\.[0-9]+)?)"))) {
            cfg.threshold = std::stod(m[1].str());
        }
        // sleep_timeout_sec (int)
        if (std::regex_search(content, m, std::regex("\"sleep_timeout_sec\"\\s*:\\s*([0-9]+)"))) {
            cfg.sleep_timeout_sec = std::stoi(m[1].str());
        }
    } else {
        std::cerr << "[config] config file not found at " << path << ", will create default\n";
    }

    // device_id 없으면 생성하고 파일에 저장
    if (cfg.device_id.empty()) {
        char host[128] = {0};
        if (gethostname(host, sizeof(host)) != 0) {
            std::snprintf(host, sizeof(host), "unknown");
        }
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        cfg.device_id = std::string(host) + "_" + std::to_string(t);
        std::cerr << "[config] generated device_id=" << cfg.device_id << "\n";
        write_config_file(path, cfg);
    }

    return cfg;
}