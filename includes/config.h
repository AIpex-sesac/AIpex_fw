#pragma once
#include <string>

struct AppConfig {
    std::string device_id = "AipexFW";
    double threshold = 0.8;
    int sleep_timeout_sec = 60;
};

AppConfig load_config(const std::string& filepath);