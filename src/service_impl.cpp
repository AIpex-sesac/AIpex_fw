#include "service_impl.h"
#include <iostream>
#include <csignal>
#include <chrono>
#include <opencv2/opencv.hpp>

// forward declaration from hailo_object_detection.cpp
extern int hailo_infer(const cv::Mat& input_frame, bool return_image, std::string& result_json, cv::Mat& result_image);

grpc::Status ComputeServiceImpl::Datastream(::grpc::ServerContext* context,
                                            ::grpc::ServerReaderWriter<data_types::ServerMessage, data_types::Command>* stream) {
    std::mutex write_mtx;
    std::atomic<bool> running{true};

    data_types::Command cmd;
    while (running.load()) {
        if (context->IsCancelled()) {
            std::cerr << "[service] context cancelled, exiting\n";
            break;
        }

        bool ok = stream->Read(&cmd);
        if (!ok) {
            std::cerr << "[service] client closed stream\n";
            break;
        }

        // std::cerr << "[service recv] cmd:\n" << cmd.DebugString() << "\n";
        // DebugString prints binary data → only log command type
        std::cerr << "[service recv] cmd type: ";
        if (cmd.has_control_action()) std::cerr << "control_action\n";
        else if (cmd.has_heartbeat()) std::cerr << "heartbeat\n";
        else if (cmd.has_camera_frame()) {
            std::cerr << "camera_frame (" << cmd.camera_frame().image_data().size() << " bytes)\n";
        }
        else if (cmd.has_detection_result()) std::cerr << "detection_result\n";
        else std::cerr << "unknown\n";

        // Handle incoming Command
        if (cmd.has_control_action()) {
            auto action = cmd.control_action().action();
            if (action == data_types::ControlAction::REBOOT) {
                std::cerr << "[service] REBOOT requested\n";
                // handle reboot
            } else if (action == data_types::ControlAction::START_STREAMING) {
                std::cerr << "[service] START_STREAMING\n";
            } else if (action == data_types::ControlAction::STOP_STREAMING) {
                std::cerr << "[service] STOP_STREAMING\n";
                running.store(false);
            }
        } else if (cmd.has_heartbeat()) {
            std::cerr << "[service] heartbeat received\n";
        } else if (cmd.has_camera_frame()) {
            // NEW: handle incoming camera frame for inference
            auto& cf = cmd.camera_frame();
            std::cerr << "[service] camera_frame received: " << cf.width() << "x" << cf.height() << "\n";

            // Decode image_data to cv::Mat
            std::vector<uint8_t> img_bytes(cf.image_data().begin(), cf.image_data().end());
            cv::Mat frame = cv::imdecode(img_bytes, cv::IMREAD_COLOR);
            if (frame.empty()) {
                std::cerr << "[service] Failed to decode image\n";
                continue;
            }

            // Run inference
            std::string result_json;
            cv::Mat result_image;
            bool return_image = false; // set true if you want annotated image back
            int ret = hailo_infer(frame, return_image, result_json, result_image);
            if (ret != 0) {
                std::cerr << "[service] hailo_infer failed\n";
                continue;
            }

            // Send response
            data_types::ServerMessage sm;
            if (!return_image) {
                // Send JSON detection result
                auto dr = sm.mutable_detection_result();
                dr->set_json(result_json);
                auto ts = dr->mutable_frame_timestamp();
                auto now = std::chrono::system_clock::now();
                ts->set_seconds(std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
            } else {
                // Send annotated image as CameraFrame
                auto out_cf = sm.mutable_camera_frame();
                std::vector<uint8_t> enc_buf;
                cv::imencode(".jpg", result_image, enc_buf);
                out_cf->set_image_data(enc_buf.data(), enc_buf.size());
                out_cf->set_width(result_image.cols);
                out_cf->set_height(result_image.rows);
                out_cf->set_format("JPEG");
                auto ts = out_cf->mutable_timestamp();
                auto now = std::chrono::system_clock::now();
                ts->set_seconds(std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
            }

            {
                std::lock_guard<std::mutex> lk(write_mtx);
                bool write_ok = stream->Write(sm);
                if (!write_ok) {
                    std::cerr << "[service] Write failed, client disconnected\n";
                    running.store(false);
                    break;
                }
            }
        }

    if (forward_client) {
        forward_client->StopStreaming();
        forward_client.reset();
    }

    std::cerr << "[service] Datastream handler exiting\n";
    return grpc::Status::OK;
}