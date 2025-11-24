#include "hailo/hailort.hpp"
#include "hailo_utils.h"
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

// Global detection threshold (can be set from main or via gRPC config)
static float g_detection_threshold = 0.5f; // default 50%

void set_detection_threshold(float threshold) {
    g_detection_threshold = threshold;
    std::cerr << "[hailo] Detection threshold set to " << g_detection_threshold << "\n";
}

float get_detection_threshold() {
    return g_detection_threshold;
}

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

    // 4b) Allocate and set output buffers for all outputs
    auto output_names = g_hailo_ctx.infer_model->get_output_names();
    if (output_names.empty()) {
        std::cerr << "[hailo] No output names found\n";
        return -1;
    }
    
    // Get output vstream infos to calculate buffer sizes
    auto output_vstream_infos = g_hailo_ctx.infer_model->hef().get_output_vstream_infos();
    if (!output_vstream_infos || output_vstream_infos->empty()) {
        std::cerr << "[hailo] Failed to get output vstream infos\n";
        return -1;
    }
    
    std::vector<std::vector<uint8_t>> output_buffers;
    for (size_t i = 0; i < output_names.size(); ++i) {
        const auto& output_name = output_names[i];
        const auto& vstream_info = output_vstream_infos->at(i);
        
        // Get actual frame size (handles NMS postprocess output correctly)
        size_t output_size = HailoRTCommon::get_frame_size(vstream_info, vstream_info.format);
        
        std::cerr << "[hailo] Output '" << output_name << "' size: " << output_size << " bytes\n";
        
        output_buffers.emplace_back(output_size);
        status = bindings.output(output_name)->set_buffer(MemoryView(output_buffers.back().data(), output_buffers.back().size()));
        if (status != HAILO_SUCCESS) {
            std::cerr << "[hailo] Failed to set output buffer for '" << output_name << "': " << status << "\n";
            return -1;
        }
    }

    // 5) Run inference (synchronous)
    status = g_hailo_ctx.configured_infer_model->run(bindings, std::chrono::milliseconds(1000));
    if (status != HAILO_SUCCESS) {
        std::cerr << "[hailo] Inference failed: " << status << "\n";
        return -1;
    }

    // 6) Get output (assumes single output for simplicity)
    //    auto output_names = g_hailo_ctx.infer_model->get_output_names();
    //    if (output_names.empty()) {
    //        std::cerr << "[hailo] No output names found\n";
    //        return -1;
    //    }
    //    auto output_name = output_names[0];
    //    
    //    auto output_buffer = bindings.output(output_name)->get_buffer();
    //    if (!output_buffer) {
    //        std::cerr << "[hailo] Failed to get output buffer\n";
    //        return -1;
    //    }

    // Output data is now in output_buffers[0] (first output)
    const auto& output_data = output_buffers[0];

    // 7) Postprocess
    // Parse NMS output to get bounding boxes
    size_t class_count = 80; // COCO dataset has 80 classes (adjust if your model differs)
    auto bboxes = parse_nms_data(output_buffers[0].data(), class_count);

    // Filter by threshold
    std::vector<NamedBbox> filtered_bboxes;
    for (const auto& bbox : bboxes) {
        if (bbox.bbox.score >= g_detection_threshold) {
            filtered_bboxes.push_back(bbox);
        }
    }

    std::cerr << "[hailo] Detected " << bboxes.size() << " objects, "
              << filtered_bboxes.size() << " above threshold " << g_detection_threshold << "\n";

    if (!return_image) {
        // Build JSON with actual detections
        result_json = "{\"detections\":[";
        bool first = true;
        for (const auto& bbox : filtered_bboxes) {
            if (!first) result_json += ",";
            first = false;

            std::string class_name = get_coco_name_from_int(static_cast<int>(bbox.class_id));
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                "{\"class\":\"%s\",\"class_id\":%d,\"score\":%.4f,\"bbox\":{\"x_min\":%.4f,\"y_min\":%.4f,\"x_max\":%.4f,\"y_max\":%.4f}}",
                class_name.c_str(),
                static_cast<int>(bbox.class_id),
                bbox.bbox.score,
                bbox.bbox.x_min,
                bbox.bbox.y_min,
                bbox.bbox.x_max,
                bbox.bbox.y_max);
            result_json += buf;
        }
        result_json += "],\"count\":";
        result_json += std::to_string(filtered_bboxes.size());
        result_json += "}";
        return 0;
    } else {
        // Draw bboxes on image
        result_image = resized.clone();
        draw_bounding_boxes(result_image, filtered_bboxes);
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