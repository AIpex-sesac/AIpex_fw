#pragma once
#include <string>
#include <cstdint>

class HailoDevice {
public:
    HailoDevice(const std::string& device_id);
    ~HailoDevice();

    bool initialize();
    void shutdown();
    void perform_inference(const std::string& input_data, std::string& output_data);
    void go_to_sleep();
    void wake_up();

    // 상태 조회
    bool is_mock() const;
    bool is_initialized() const;
private:
    std::string device_id_;
    void* lib_handle_ = nullptr;
    bool initialized_ = false;
    bool mock_mode_ = false;

    using fn_init_t = int(*)(const char*);
    using fn_deinit_t = void(*)(void);
    using fn_infer_t = int(*)(const uint8_t*, size_t, char*, size_t);

    fn_init_t fn_init_ = nullptr;
    fn_deinit_t fn_deinit_ = nullptr;
    fn_infer_t fn_infer_ = nullptr;
};