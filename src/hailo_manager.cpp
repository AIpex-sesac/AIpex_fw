#include "hailo_manager.h"
#include <iostream>
#include <cstdlib>
#include <string>

using namespace hailort;

HailoManager& HailoManager::instance() {
    static HailoManager inst;
    return inst;
}

int HailoManager::init(const char* hef_path) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (configured_infer_model_) return 0;

    const char* path = hef_path;
    if (!path) path = std::getenv("HEF_PATH");
    if (!path) path = "/home/pi/hailo/best.hef";

    auto vdevice_exp = VDevice::create();
    if (!vdevice_exp) {
        std::cerr << "[HailoManager] VDevice::create failed: " << vdevice_exp.status() << "\n";
        return -1;
    }
    vdevice_ = vdevice_exp.release();

    auto infer_model_exp = vdevice_->create_infer_model(path);
    if (!infer_model_exp) {
        std::cerr << "[HailoManager] create_infer_model failed: " << infer_model_exp.status() << "\n";
        vdevice_.reset();
        return -1;
    }
    infer_model_ = infer_model_exp.release();

    auto input_vstream_infos = infer_model_->hef().get_input_vstream_infos();
    if (!input_vstream_infos || input_vstream_infos->empty()) {
        std::cerr << "[HailoManager] get_input_vstream_infos failed\n";
        infer_model_.reset();
        vdevice_.reset();
        return -1;
    }
    input_shape_ = input_vstream_infos->at(0).shape;
    input_frame_size_ = input_shape_.height * input_shape_.width * input_shape_.features;

    infer_model_->set_batch_size(1);
    auto configured_exp = infer_model_->configure();
    if (!configured_exp) {
        std::cerr << "[HailoManager] configure failed: " << configured_exp.status() << "\n";
        infer_model_.reset();
        vdevice_.reset();
        return -1;
    }
    configured_infer_model_ = std::make_shared<ConfiguredInferModel>(configured_exp.release());

    std::cerr << "[HailoManager] initialized HEF=" << path << "\n";
    return 0;
}

void HailoManager::cleanup() {
    std::lock_guard<std::mutex> lk(mtx_);
    configured_infer_model_.reset();
    infer_model_.reset();
    vdevice_.reset();
    input_frame_size_ = 0;
    std::cerr << "[HailoManager] cleaned up\n";
}

std::shared_ptr<InferModel> HailoManager::get_infer_model() {
    std::lock_guard<std::mutex> lk(mtx_);
    return infer_model_;
}
std::shared_ptr<ConfiguredInferModel> HailoManager::get_configured_infer_model() {
    std::lock_guard<std::mutex> lk(mtx_);
    return configured_infer_model_;
}
hailo_3d_image_shape_t HailoManager::get_input_shape() {
    std::lock_guard<std::mutex> lk(mtx_);
    return input_shape_;
}
size_t HailoManager::get_input_frame_size() {
    std::lock_guard<std::mutex> lk(mtx_);
    return input_frame_size_;
}
bool HailoManager::is_lowlight_only() const {
    const char* lle = std::getenv("LOWLIGHT_ENHANCE");
    if (!lle) return false;
    return std::string(lle) == "1";
}

bool HailoManager::get_or_create_model(const std::string& hef_path,
        std::shared_ptr<InferModel>& out_infer,
        std::shared_ptr<ConfiguredInferModel>& out_configured)
{
    std::lock_guard<std::mutex> lk(mtx_);
    auto it_cfg = extra_configured_models_.find(hef_path);
    if (it_cfg != extra_configured_models_.end()) {
        out_configured = it_cfg->second;
        out_infer = extra_infer_models_[hef_path];
        return true;
    }
    if (!vdevice_) {
        std::cerr << "[HailoManager] VDevice not initialized\n";
        return false;
    }
    auto infer_exp = vdevice_->create_infer_model(hef_path.c_str());
    if (!infer_exp) {
        std::cerr << "[HailoManager] create_infer_model failed for " << hef_path << " : " << infer_exp.status() << "\n";
        return false;
    }
    std::shared_ptr<InferModel> infer = std::shared_ptr<InferModel>(infer_exp.release());
    infer->set_batch_size(1);
    auto configured_exp = infer->configure();
    if (!configured_exp) {
        std::cerr << "[HailoManager] configure failed for " << hef_path << " : " << configured_exp.status() << "\n";
        return false;
    }
    std::shared_ptr<ConfiguredInferModel> cfg = std::make_shared<ConfiguredInferModel>(configured_exp.release());

    // store to keep lifetime
    extra_infer_models_[hef_path] = infer;
    extra_configured_models_[hef_path] = cfg;

    out_infer = infer;
    out_configured = cfg;
    std::cerr << "[HailoManager] Loaded extra HEF: " << hef_path << "\n";
    return true;
}