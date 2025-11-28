#include "hailo/hailort.hpp"
#include "hailo_utils.h"
#include <opencv2/opencv.hpp>
#include <iostream>
#include <vector>
#include <memory>
#include <cstring>
#include <chrono>

#define LLE_HEF "/home/pi/hailo/zero_dce_pp.hef" // 이미 정의되어 있으면 사용

using namespace hailort;

// Hailo context (using C++ API objects)
#include "hailo_manager.h"

// Process a single frame with the lowlight enhancement model.
// Returns 0 on success, -1 on failure.
// result_image will contain the enhanced image (BGR, same size as input_frame) on success.
int hailo_lowlight_process(const cv::Mat& input_frame, cv::Mat& result_image) {
    std::shared_ptr<hailort::InferModel> infer_model;
    std::shared_ptr<hailort::ConfiguredInferModel> configured;
    if (!HailoManager::instance().get_or_create_model(LLE_HEF, infer_model, configured)) {
        std::cerr << "[hailo_ll] failed to load lowlight HEF model\n";
        return -1;
    }
    if (nullptr == configured || nullptr == infer_model) {
        std::cerr << "[hailo_ll] configured_infer_model or infer_model is null\n";
        return -1;
    }
    // lowlight 모델의 입력 shape을 직접 얻고 싶으면 infer_model->hef().get_input_vstream_infos() 사용
    auto input_vinfos = infer_model->hef().get_input_vstream_infos();
    if (!input_vinfos || input_vinfos->empty()) {
        std::cerr << "[hailo_ll] failed to get input vstream infos for lowlight model\n";
        return -1;
    }
    hailo_3d_image_shape_t input_shape = input_vinfos->at(0).shape;
    int model_h = input_shape.height;
    int model_w = input_shape.width;
    // preprocess: resize to model input and convert BGR->RGB
    cv::Mat resized;
    cv::resize(input_frame, resized, cv::Size(model_w, model_h));
    cv::Mat rgb;
    cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);

    size_t input_frame_size = static_cast<size_t>(model_h) * model_w * input_shape.features;
    std::vector<uint8_t> input_data(input_frame_size);
    if (rgb.isContinuous()) {
        std::memcpy(input_data.data(), rgb.data, input_data.size());
    } else {
        size_t offset = 0;
        for (int r = 0; r < rgb.rows; ++r) {
            std::memcpy(input_data.data() + offset, rgb.ptr(r), rgb.cols * rgb.elemSize());
            offset += rgb.cols * rgb.elemSize();
        }
    }

    // 3) Create bindings
    auto bindings_exp = configured->create_bindings();
    if (!bindings_exp) {
        std::cerr << "[hailo_ll] Failed to create bindings: " << bindings_exp.status() << "\n";
        return -1;
    }
    auto bindings = bindings_exp.release();

    // 4) Set input buffer
    auto input_name = infer_model->get_input_names()[0];
    hailo_status status = bindings.input(input_name)->set_buffer(MemoryView(input_data.data(), input_data.size()));
    if (status != HAILO_SUCCESS) {
        std::cerr << "[hailo_ll] Failed to set input buffer: " << status << "\n";
        return -1;
    }

    // 5) Allocate outputs
    auto output_names = infer_model->get_output_names();
    if (output_names.empty()) {
        std::cerr << "[hailo_ll] No output names found\n";
        return -1;
    }
    auto output_vstream_infos = infer_model->hef().get_output_vstream_infos();
    if (!output_vstream_infos || output_vstream_infos->empty()) {
        std::cerr << "[hailo_ll] Failed to get output vstream infos\n";
        return -1;
    }

    // 예상 이미지 바이트 수 (model_h * model_w * 3 channels)
    size_t expected_u8 = static_cast<size_t>(model_h) * model_w * 3;
    std::vector<std::vector<uint8_t>> output_buffers;
    for (size_t i = 0; i < output_names.size(); ++i) {
        const auto& vstream_info = output_vstream_infos->at(i);
        size_t output_size = HailoRTCommon::get_frame_size(vstream_info, vstream_info.format);
        // 할당할 크기는 모델에서 요구하는 output_size와 이미지 재구성에 필요한 expected_u8 중 큰 값으로 한다.
        size_t alloc_size = std::max(output_size, expected_u8);
        output_buffers.emplace_back(alloc_size);
        status = bindings.output(output_names[i])->set_buffer(MemoryView(output_buffers.back().data(), output_buffers.back().size()));
        if (status != HAILO_SUCCESS) {
            std::cerr << "[hailo_ll] Failed to set output buffer for '" << output_names[i] << "': " << status << "\n";
            return -1;
        }
        std::cerr << "[hailo_ll] Allocated output[" << i << "] buffer size=" << alloc_size
                  << " (model reported " << output_size << ", expected_image " << expected_u8 << ")\n";
    }

    // 6) Run inference (synchronous)
    status = configured->run(bindings, std::chrono::milliseconds(1000));
    if (status != HAILO_SUCCESS) {
        std::cerr << "[hailo_ll] Inference failed: " << status << "\n";
        return -1;
    }

    // 7) Reconstruct output image from first output buffer
    // Find the best output buffer that can contain an image (debug/log sizes)
    size_t expected_f32 = expected_u8 * sizeof(float);
    int chosen_idx = -1;
    for (size_t i = 0; i < output_buffers.size(); ++i) {
        auto sz = output_buffers[i].size();
        std::cerr << "[hailo_ll] output[" << i << "] size=" << sz << " bytes\n";
        if (sz >= expected_u8) {
            chosen_idx = static_cast<int>(i);
            break;
        }
    }
    if (chosen_idx < 0) {
        // No sufficiently large buffer — log largest and fail
        size_t maxsz = 0; int maxi = -1;
        for (size_t i = 0; i < output_buffers.size(); ++i) {
            if (output_buffers[i].size() > maxsz) { maxsz = output_buffers[i].size(); maxi = static_cast<int>(i); }
        }
        std::cerr << "[hailo_ll] No output buffer large enough for image. largest idx=" << maxi
                  << " size=" << maxsz << " expected_u8=" << expected_u8 << "\n";
        return -1;
    }

    const auto &out_buf = output_buffers[chosen_idx];
    // Try UINT8 RGB first
    if (out_buf.size() >= expected_u8) {
        cv::Mat out_rgb(model_h, model_w, CV_8UC3, const_cast<uint8_t*>(out_buf.data()));
        cv::Mat out_bgr;
        cv::cvtColor(out_rgb, out_bgr, cv::COLOR_RGB2BGR);
        cv::resize(out_bgr, result_image, input_frame.size());
        return 0;
    }
    // Try FP32 (float32 RGB) case
    if (out_buf.size() >= expected_f32) {
        cv::Mat out_f(model_h, model_w, CV_32FC3, const_cast<float*>(reinterpret_cast<const float*>(out_buf.data())));
        // clamp/scale -> 0..255
        cv::Mat out_u8;
        out_f.convertTo(out_u8, CV_8UC3, 255.0);
        cv::Mat out_bgr;
        cv::cvtColor(out_u8, out_bgr, cv::COLOR_RGB2BGR);
        cv::resize(out_bgr, result_image, input_frame.size());
        return 0;
    }

    std::cerr << "[hailo_ll] Unsupported output buffer format/size (" << out_buf.size() << " bytes)\n";
    return -1;
}

// Exported init wrapper for main to call (keeps symbol used by main)
int hailo_lowlight_enhance(int argc, char** argv) {
    // main에서 이미 HailoManager::init()을 호출하도록 변경함
    const char* hef_path = std::getenv("HEF_PATH");
    if (!hef_path) hef_path = LLE_HEF;
    std::cerr << "[hailo_ll] HEF path: " << hef_path << " (expect initialized by main)\n";
    if (nullptr == HailoManager::instance().get_configured_infer_model()) {
        std::cerr << "[hailo_ll] ERROR: HailoManager not initialized. Call HailoManager::instance().init() from main\n";
        return -1;
    }

    std::cerr << "[hailo_ll] Hailo ready. Waiting for gRPC requests...\n";
    return 0;
}