#include "service_impl.h"
#include <iostream>
#include <csignal>
#include <chrono>
#include <opencv2/opencv.hpp>
#include "grpc_client.h"
#include "hailo_manager.h"

// forward decls (implementations live in hailo_lowlight_enhance.cpp / hailo_object_detection.cpp)
extern int hailo_lowlight_process(const cv::Mat& input_frame, cv::Mat& result_image);
extern int hailo_infer(const cv::Mat& input_frame, bool return_image, std::string& result_json, cv::Mat& result_image);

grpc::Status ComputeServiceImpl::Datastream(::grpc::ServerContext* context,
    ::grpc::ServerReaderWriter<data_types::ServerMessage, data_types::Command>* stream) {

    std::mutex write_mtx;
    std::atomic<bool> running{true};

    // Optional: forwarding target from env, create persistent client for this Datastream handler
    std::string forward_target;
    if (const char* ft = std::getenv("FORWARD_TARGET")) forward_target = ft;
    std::unique_ptr<GrpcClient> forward_client;
    if (!forward_target.empty()) {
        forward_client = std::make_unique<GrpcClient>(forward_target);
        if (!forward_client->StartStreaming()) {
            std::cerr << "[service] failed to start forward client to " << forward_target << "\n";
            forward_client.reset();
        } else {
            std::cerr << "[service] forwarding enabled -> " << forward_target << "\n";
        }
    }

    data_types::Command cmd;
    while (running.load()) {
        if (context->IsCancelled()) {
            std::cerr << "[service] context cancelled\n";
            break;
        }
        bool ok = stream->Read(&cmd);
        if (!ok) {
            std::cerr << "[service] client closed stream\n";
            break;
        }

        // === handle camera frame (single normalized branch) ===
        if (cmd.has_camera_frame()) {
            auto& cf_in = cmd.camera_frame();
            std::vector<uint8_t> img_bytes(cf_in.image_data().begin(), cf_in.image_data().end());
            cv::Mat frame = cv::imdecode(img_bytes, cv::IMREAD_COLOR);
            if (frame.empty()) {
                std::cerr << "[service] Failed to decode image\n";
                continue;
            }

            bool lowlight_only = HailoManager::instance().is_lowlight_only();
            if (lowlight_only) {
                // lowlight-only: send enhanced image (or original on failure)
                cv::Mat lle_out;
                int lle_ret = hailo_lowlight_process(frame, lle_out);
                data_types::ServerMessage sm;
                std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 85};
                std::vector<uint8_t> outbuf;
                if (lle_ret == 0 && !lle_out.empty()) {
                    if (!cv::imencode(".jpg", lle_out, outbuf, params)) {
                        std::cerr << "[service] Failed to encode enhanced image\n";
                        outbuf.clear();
                    }
                } else {
                    std::cerr << "[service] lowlight enhancement failed, using original frame\n";
                    if (!cv::imencode(".jpg", frame, outbuf, params)) {
                        std::cerr << "[service] Failed to encode original frame\n";
                        outbuf.clear();
                    }
                }
                if (!outbuf.empty()) {
                    data_types::CameraFrame out_cf;
                    out_cf.set_image_data(reinterpret_cast<const char*>(outbuf.data()), outbuf.size());
                    out_cf.set_width(static_cast<uint32_t>( (lle_ret==0 && !lle_out.empty()) ? lle_out.cols : frame.cols ));
                    out_cf.set_height(static_cast<uint32_t>( (lle_ret==0 && !lle_out.empty()) ? lle_out.rows : frame.rows ));
                    out_cf.set_format("JPEG");
                    if (cf_in.camera_id()) out_cf.set_camera_id(cf_in.camera_id());
                    if (cf_in.has_timestamp()) out_cf.mutable_timestamp()->CopyFrom(cf_in.timestamp());
                    sm.mutable_camera_frame()->CopyFrom(out_cf);
                } else {
                    std::cerr << "[service] No image to send\n";
                }

                std::lock_guard<std::mutex> lk(write_mtx);
                if (!stream->Write(sm)) {
                    std::cerr << "[service] Write failed, client disconnected\n";
                    running.store(false);
                    break;
                }
                continue;
            } else {
                // detection-only: run detection and send JSON result only
                std::string result_json;
                cv::Mat dummy_img;
                int det_ret = hailo_infer(frame, /*return_image=*/false, result_json, dummy_img);
                if (det_ret != 0) {
                    std::cerr << "[service] hailo_infer failed\n";
                    continue;
                }
                data_types::ServerMessage sm;
                if (!result_json.empty()) {
                    auto dr = sm.mutable_detection_result();
                    dr->set_json(result_json);
                    if (cf_in.camera_id()) dr->set_camera_id(cf_in.camera_id());
                    if (cf_in.has_timestamp()) dr->mutable_frame_timestamp()->CopyFrom(cf_in.timestamp());
                }
                std::lock_guard<std::mutex> lk(write_mtx);
                if (!stream->Write(sm)) {
                    std::cerr << "[service] Write failed, client disconnected\n";
                    running.store(false);
                    break;
                }
                continue;
            }
        }

        // === other command types ===
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
                break;
            }
        } else if (cmd.has_heartbeat()) {
            std::cerr << "[service] heartbeat received\n";
        } else {
            // unknown/unsupported command
        }
    }

    if (forward_client) {
        forward_client->StopStreaming();
        forward_client.reset();
    }

    std::cerr << "[service] Datastream handler exiting\n";
    return grpc::Status::OK;
}