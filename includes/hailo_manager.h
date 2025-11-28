#pragma once
#include "hailo/hailort.hpp"
#include <map>
#include <memory>
#include <mutex>
#include <string>

class HailoManager {
public:
    static HailoManager& instance();

    int init(const char* hef_path = nullptr);
    void cleanup();

    std::shared_ptr<hailort::InferModel> get_infer_model();
    std::shared_ptr<hailort::ConfiguredInferModel> get_configured_infer_model();
    hailo_3d_image_shape_t get_input_shape();
    size_t get_input_frame_size();

    // load or return an already-loaded model (keep InferModel alive)
    // returns true on success, and fills out_infer/out_configured
    bool get_or_create_model(const std::string& hef_path,
                             std::shared_ptr<hailort::InferModel>& out_infer,
                             std::shared_ptr<hailort::ConfiguredInferModel>& out_configured);

    // 환경변수/설정으로 lowlight-only 동작 여부 확인
    bool is_lowlight_only() const;

private:
    HailoManager() = default;
    ~HailoManager() = default;
    HailoManager(const HailoManager&) = delete;
    HailoManager& operator=(const HailoManager&) = delete;

    std::mutex mtx_;
    std::unique_ptr<hailort::VDevice> vdevice_;
    std::shared_ptr<hailort::InferModel> infer_model_;
    std::shared_ptr<hailort::ConfiguredInferModel> configured_infer_model_;
    hailo_3d_image_shape_t input_shape_{};
    size_t input_frame_size_ = 0;
    std::map<std::string, std::shared_ptr<hailort::InferModel>> extra_infer_models_;
    std::map<std::string, std::shared_ptr<hailort::ConfiguredInferModel>> extra_configured_models_;
};