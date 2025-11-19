#include "hailo.h"
#include <iostream>
#include <dlfcn.h>
#include <cstdlib>
#include <cstring>

HailoDevice::HailoDevice(const std::string& device_id)
    : device_id_(device_id) {}

HailoDevice::~HailoDevice() {
    shutdown();
}

static void* try_load_lib() {
    const char* path = std::getenv("HAILO_LIB_PATH");
    if (path && path[0]) {
        void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (h) return h;
        std::cerr << "[Hailo] dlopen(" << path << ") failed: " << dlerror() << "\n";
    }
    void* h = dlopen("libhailort.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) std::cerr << "[Hailo] dlopen(libhailort.so) failed: " << dlerror() << "\n";
    return h;
}

bool HailoDevice::initialize() {
    // mock 모드 확인 (테스트용)
    const char* mock_env = std::getenv("HAILO_MOCK");
    if (mock_env && mock_env[0]) {
        mock_mode_ = true;
        initialized_ = true;
        std::cerr << "[Hailo] running in MOCK mode (HAILO_MOCK set)\n";
        return true;
    }

    if (initialized_) return true;

    lib_handle_ = try_load_lib();
    if (!lib_handle_) {
        std::cerr << "[Hailo] Hailo runtime not available\n";
        initialized_ = false;
        return false;
    }

    fn_init_ = reinterpret_cast<fn_init_t>(dlsym(lib_handle_, "hailo_runtime_init"));
    fn_deinit_ = reinterpret_cast<fn_deinit_t>(dlsym(lib_handle_, "hailo_runtime_deinit"));
    fn_infer_ = reinterpret_cast<fn_infer_t>(dlsym(lib_handle_, "hailo_run_inference"));

    if (!fn_init_ || !fn_deinit_ || !fn_infer_) {
        std::cerr << "[Hailo] required symbols not found in library: " << dlerror() << "\n";
        dlclose(lib_handle_);
        lib_handle_ = nullptr;
        return false;
    }

    int rc = fn_init_(device_id_.c_str());
    if (rc != 0) {
        std::cerr << "[Hailo] runtime init failed rc=" << rc << "\n";
        dlclose(lib_handle_);
        lib_handle_ = nullptr;
        return false;
    }

    initialized_ = true;
    std::cerr << "[Hailo] initialized OK\n";
    return true;
}

void HailoDevice::shutdown() {
    if (mock_mode_) {
        initialized_ = false;
        mock_mode_ = false;
        std::cerr << "[Hailo] mock shutdown\n";
        return;
    }
    if (!initialized_) {
        if (lib_handle_) { dlclose(lib_handle_); lib_handle_ = nullptr; }
        return;
    }

    if (fn_deinit_) fn_deinit_();
    if (lib_handle_) { dlclose(lib_handle_); lib_handle_ = nullptr; }
    initialized_ = false;
    std::cerr << "[Hailo] shutdown complete\n";
}

void HailoDevice::perform_inference(const std::string& input_data, std::string& output_data) {
    if (mock_mode_ || !initialized_ || !fn_infer_) {
        output_data = "detected: (mock) for " + input_data;
        return;
    }

    const uint8_t* in_buf = reinterpret_cast<const uint8_t*>(input_data.data());
    size_t in_len = input_data.size();
    constexpr size_t OUT_MAX = 8192;
    char out_buf[OUT_MAX];
    std::memset(out_buf, 0, OUT_MAX);

    int rc = fn_infer_(in_buf, in_len, out_buf, OUT_MAX);
    if (rc != 0) {
        std::cerr << "[Hailo] inference failed rc=" << rc << "\n";
        output_data = "error";
        return;
    }
    output_data = std::string(out_buf);
}

void HailoDevice::go_to_sleep() {
    if (mock_mode_) {
        std::cerr << "[Hailo] mock go_to_sleep\n";
        initialized_ = false;
        return;
    }
    if (!initialized_) return;
    using fn_sleep_t = int(*)(void);
    fn_sleep_t fn_sleep = reinterpret_cast<fn_sleep_t>(dlsym(lib_handle_, "hailo_enter_low_power"));
    if (fn_sleep) {
        int rc = fn_sleep();
        std::cerr << "[Hailo] enter_low_power rc=" << rc << "\n";
    } else {
        shutdown();
    }
}

void HailoDevice::wake_up() {
    if (mock_mode_) {
        std::cerr << "[Hailo] mock wake_up\n";
        initialized_ = true;
        return;
    }
    if (initialized_) return;
    initialize();
}

bool HailoDevice::is_mock() const { return mock_mode_; }
bool HailoDevice::is_initialized() const { return initialized_; }