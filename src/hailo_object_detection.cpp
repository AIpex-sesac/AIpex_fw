#include "hailo/hailort.hpp"
#include <opencv2/opencv.hpp>
#include <iostream>
#include <vector>
#include <memory>

#define HEF_FILE "/home/pi/hailo/best.hef"

using namespace hailort;

// Hailo context (using C++ API objects)
struct HailoContext {
    std::unique_ptr<VDevice> vdevice;
    std::shared_ptr<InferModel> infer_model;
    std::shared_ptr<ConfiguredInferModel> configured_infer_model;
    hailo_3d_image_shape_t input_shape;
    size_t input_frame_size = 0;
};

static HailoContext g_hailo_ctx;

// Initialize Hailo device & network group once
int hailo_init(const char* hef_path) {
    // 1) Create VDevice
    auto vdevice_exp = VDevice::create();
    if (!vdevice_exp) {
        std::cerr << "[hailo] Failed to create VDevice: " << vdevice_exp.status() << "\n";
        return -1;
    }
    g_hailo_ctx.vdevice = vdevice_exp.release();

    // 2) Create InferModel from HEF
    auto infer_model_exp = g_hailo_ctx.vdevice->create_infer_model(hef_path);
    if (!infer_model_exp) {
        std::cerr << "[hailo] Failed to create infer model: " << infer_model_exp.status() << "\n";
        return -1;
    }
    g_hailo_ctx.infer_model = infer_model_exp.release();

    // 3) Get input shape
    auto input_vstream_infos = g_hailo_ctx.infer_model->hef().get_input_vstream_infos();
    if (!input_vstream_infos || input_vstream_infos->empty()) {
        std::cerr << "[hailo] Failed to get input vstream infos\n";
        return -1;
    }
    g_hailo_ctx.input_shape = input_vstream_infos->at(0).shape;
    g_hailo_ctx.input_frame_size = g_hailo_ctx.input_shape.height 
                                 * g_hailo_ctx.input_shape.width 
                                 * g_hailo_ctx.input_shape.features;

    std::cerr << "[hailo] Input shape: " << g_hailo_ctx.input_shape.height << "x"
              << g_hailo_ctx.input_shape.width << "x" << g_hailo_ctx.input_shape.features
              << " (size=" << g_hailo_ctx.input_frame_size << " bytes)\n";

    // 4) Configure model with batch_size=1
    g_hailo_ctx.infer_model->set_batch_size(1);
    auto configured_infer_model_exp = g_hailo_ctx.infer_model->configure();
    if (!configured_infer_model_exp) {
        std::cerr << "[hailo] Failed to configure infer model: " << configured_infer_model_exp.status() << "\n";
        return -1;
    }
    // release() returns ConfiguredInferModel value, wrap in shared_ptr
    g_hailo_ctx.configured_infer_model = std::make_shared<ConfiguredInferModel>(configured_infer_model_exp.release());

    std::cerr << "[hailo] Initialized successfully\n";
    return 0;
}

void hailo_cleanup() {
    g_hailo_ctx.configured_infer_model.reset();
    g_hailo_ctx.infer_model.reset();
    g_hailo_ctx.vdevice.reset();
    std::cerr << "[hailo] Cleanup complete\n";
}

// Run inference on a single frame (cv::Mat), return detection JSON or annotated image
// Returns 0 on success, -1 on failure
int hailo_infer(const cv::Mat& input_frame, bool return_image, std::string& result_json, cv::Mat& result_image) {
    // 1) Preprocess: resize to model input size
    int model_h = g_hailo_ctx.input_shape.height;
    int model_w = g_hailo_ctx.input_shape.width;
    
    cv::Mat resized;
    cv::resize(input_frame, resized, cv::Size(model_w, model_h));
    
    // Convert BGR→RGB if needed
    cv::Mat rgb;
    cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);

    // 2) Prepare input buffer (contiguous)
    std::vector<uint8_t> input_data(g_hailo_ctx.input_frame_size);
    if (rgb.isContinuous()) {
        std::memcpy(input_data.data(), rgb.data, g_hailo_ctx.input_frame_size);
    } else {
        size_t offset = 0;
        for (int r = 0; r < rgb.rows; ++r) {
            std::memcpy(input_data.data() + offset, rgb.ptr(r), rgb.cols * rgb.elemSize());
            offset += rgb.cols * rgb.elemSize();
        }
    }

    // 3) Create bindings for this inference
    auto bindings_exp = g_hailo_ctx.configured_infer_model->create_bindings();
    if (!bindings_exp) {
        std::cerr << "[hailo] Failed to create bindings: " << bindings_exp.status() << "\n";
        return -1;
    }
    auto bindings = bindings_exp.release();

    // 4) Get input name (assumes single input)
    auto input_name = g_hailo_ctx.infer_model->get_input_names()[0];
    
    // Set input buffer
    hailo_status status = bindings.input(input_name)->set_buffer(MemoryView(input_data.data(), input_data.size()));
    if (status != HAILO_SUCCESS) {
        std::cerr << "[hailo] Failed to set input buffer: " << status << "\n";
        return -1;
    }

    // 5) Run inference (synchronous)
    // run() is synchronous and returns hailo_status directly
    status = g_hailo_ctx.configured_infer_model->run(bindings, std::chrono::milliseconds(1000));
    if (status != HAILO_SUCCESS) {
        std::cerr << "[hailo] Inference failed: " << status << "\n";
        return -1;
    }

    // 6) Get output (assumes single output for simplicity)
    auto output_names = g_hailo_ctx.infer_model->get_output_names();
    if (output_names.empty()) {
        std::cerr << "[hailo] No output names found\n";
        return -1;
    }
    auto output_name = output_names[0];
    
    auto output_buffer = bindings.output(output_name)->get_buffer();
    if (!output_buffer) {
        std::cerr << "[hailo] Failed to get output buffer\n";
        return -1;
    }

    // 7) Postprocess
    // TODO: parse NMS output properly (integrate parse_nms_data from hailo_utils.cpp)
    // For now, dummy JSON
    if (!return_image) {
        result_json = "{\"detections\":[";
        size_t preview_len = std::min<size_t>(10, output_buffer.value().size());
        for (size_t i = 0; i < preview_len; ++i) {
            if (i > 0) result_json += ",";
            result_json += std::to_string((int)output_buffer.value().data()[i]);
        }
        result_json += "],\"count\":0}";
        return 0;
    } else {
        result_image = resized.clone();
        cv::putText(result_image, "Inference OK", cv::Point(10, 30), 
                    cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0,255,0), 2);
        return 0;
    }
}

// Entry point for gRPC thread (called from main or service_impl)
int hailo_object_detection(int argc, char** argv) {
    const char* hef_path = std::getenv("HEF_PATH");
    if (!hef_path) hef_path = HEF_FILE;

    std::cerr << "[hailo_det] Initializing Hailo with HEF: " << hef_path << "\n";
    if (hailo_init(hef_path) != 0) {
        std::cerr << "[hailo_det] Hailo init failed\n";
        return -1;
    }

    std::cerr << "[hailo_det] Hailo ready. Waiting for gRPC requests...\n";
    return 0;
}