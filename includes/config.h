#pragma once
#include <string>

struct AppConfig {
    std::string device_id = "raspberry_pi_01";
    double threshold = 0.8;
    int sleep_timeout_sec = 60;
};

AppConfig load_config(const std::string& filepath);