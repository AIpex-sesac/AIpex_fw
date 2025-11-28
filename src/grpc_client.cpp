#include "grpc_client.h"
#include "ComputeService.grpc.pb.h"
#include "wakeup.grpc.pb.h"
#include "data_types.pb.h"
#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/message.h>
#include <google/protobuf/descriptor.h>
#include <iostream>
#include <sstream>
#include <regex>
#include <chrono>
#include <condition_variable>
#include <opencv2/opencv.hpp>

// Helper: parse simple arrays [x,y,w,h] or [x,y,w,h,score] in JSON-like string
static std::vector<GrpcClient::BBox> parse_bboxes_from_json(const std::string& s) {
    std::vector<GrpcClient::BBox> res;
    if (s.empty()) return res;

    try {
        // 1) Find detection objects that contain a "bbox" block
        std::regex det_re("\\{[^{}]*\"bbox\"\\s*:\\s*\\{[^{}]*\\}[^{}]*\\}");
        std::smatch dm;
        std::string::const_iterator start = s.cbegin();
        while (std::regex_search(start, s.cend(), dm, det_re)) {
            std::string det_block = dm.str();
            GrpcClient::BBox b{};
            std::smatch m;

            // try x_min,y_min,x_max,y_max
            std::regex rxmin("\"x_min\"\\s*:\\s*([-+]?[0-9]*\\.?[0-9]+)");
            std::regex rymin("\"y_min\"\\s*:\\s*([-+]?[0-9]*\\.?[0-9]+)");
            std::regex rxmax("\"x_max\"\\s*:\\s*([-+]?[0-9]*\\.?[0-9]+)");
            std::regex rymax("\"y_max\"\\s*:\\s*([-+]?[0-9]*\\.?[0-9]+)");
            if (std::regex_search(det_block, m, rxmin)) b.x = std::stof(m[1].str());
            if (std::regex_search(det_block, m, rymin)) b.y = std::stof(m[1].str());
            float x_max = 0.0f, y_max = 0.0f;
            if (std::regex_search(det_block, m, rxmax)) x_max = std::stof(m[1].str());
            if (std::regex_search(det_block, m, rymax)) y_max = std::stof(m[1].str());
            if (x_max > 0.0f) b.w = x_max - b.x;
            if (y_max > 0.0f) b.h = y_max - b.y;

            // try bbox as array inside object: "bbox":[x,y,w,h]
            if ((b.w <= 0.0f || b.h <= 0.0f)) {
                std::regex bbox_arr("\"bbox\"\\s*:\\s*\\[\\s*([-+]?[0-9]*\\.?[0-9]+)\\s*,\\s*([-+]?[0-9]*\\.?[0-9]+)\\s*,\\s*([-+]?[0-9]*\\.?[0-9]+)\\s*,\\s*([-+]?[0-9]*\\.?[0-9]+)");
                if (std::regex_search(det_block, m, bbox_arr)) {
                    b.x = std::stof(m[1].str());
                    b.y = std::stof(m[2].str());
                    b.w = std::stof(m[3].str());
                    b.h = std::stof(m[4].str());
                }
            }

            // class & score
            std::regex class_re("\"class\"\\s*:\\s*\"([^\"]+)\"");
            if (std::regex_search(det_block, m, class_re)) b.label = m[1].str();
            std::regex score_re("\"score\"\\s*:\\s*([-+]?[0-9]*\\.?[0-9]+)");
            if (std::regex_search(det_block, m, score_re)) b.score = std::stof(m[1].str());

            // push if valid
            if (b.w > 0.0f && b.h > 0.0f) {
                // clamp reasonable normalized values (if they look normalized)
                if (b.x < 0.0f) b.x = 0.0f;
                if (b.y < 0.0f) b.y = 0.0f;
                res.push_back(b);
            }

            start = dm.suffix().first;
        }

        // 2) Fallback: find any [x,y,w,h] arrays anywhere (multiple)
        if (res.empty()) {
            std::regex arr_re("\\[\\s*([-+]?[0-9]*\\.?[0-9]+)\\s*,\\s*([-+]?[0-9]*\\.?[0-9]+)\\s*,\\s*([-+]?[0-9]*\\.?[0-9]+)\\s*,\\s*([-+]?[0-9]*\\.?[0-9]+)(?:\\s*,\\s*([-+]?[0-9]*\\.?[0-9]+))?\\s*\\]");
            std::smatch am;
            start = s.cbegin();
            while (std::regex_search(start, s.cend(), am, arr_re)) {
                GrpcClient::BBox b{};
                b.x = std::stof(am[1].str());
                b.y = std::stof(am[2].str());
                b.w = std::stof(am[3].str());
                b.h = std::stof(am[4].str());
                if (am.size() > 5 && am[5].matched) b.score = std::stof(am[5].str());
                if (b.w > 0.0f && b.h > 0.0f) res.push_back(b);
                start = am.suffix().first;
            }
        }
    } catch (...) {
        // best-effort: return whatever parsed so far
    }
    return res;
}

class GrpcClient::Impl {
public:
    Impl(std::shared_ptr<grpc::Channel> ch)
      : channel_(ch), stub_(compute::ComputeService::NewStub(ch)), running_(false),
        sent_frames_(0), received_results_(0)
    {}

    // Start the bi-directional stream and reader thread
    bool Start() {
        // already running?
        if (running_.exchange(true)) return true;

        // create context and stream
        context_ = std::make_unique<grpc::ClientContext>();
        stream_ = stub_->Datastream(context_.get());
        if (!stream_) {
            std::cerr << "[client] Failed to create Datastream\n";
            running_.store(false);
            return false;
        }

        // launch reader thread
        reader_thread_ = std::thread(&Impl::ReaderLoop, this);
        return true;
    }
    
    ~Impl() { Stop(); }

    void ReaderLoop() {
        data_types::ServerMessage sm;
        std::cerr << "[client] reader_thread started\n";
        while (stream_->Read(&sm)) {
            // 기본 Debug 출력
            // std::cout << "[client recv] ServerMessage Debug:\n" << sm.DebugString() << std::endl;

            // count results if detection_result present
            if (sm.has_detection_result()) {
                received_results_.fetch_add(1, std::memory_order_relaxed);

                // try to extract JSON-like field named "json" (reflection) OR detection_result may be a message with string field
                std::string jstr;
                // reflection attempt
                const google::protobuf::Message& dr_msg = sm.detection_result();
                const google::protobuf::Descriptor* desc = dr_msg.GetDescriptor();
                const google::protobuf::Reflection* refl = dr_msg.GetReflection();
                const char* candidates[] = { "json", "json_str", "json_payload", "payload" };
                for (const char* fname : candidates) {
                    const auto* fd = desc->FindFieldByName(fname);
                    if (fd && fd->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING) {
                        jstr = refl->GetString(dr_msg, fd);
                        break;
                    }
                }
                // fallback: if detection_result.DebugString contains "json:" substring, try to extract after it
                if (jstr.empty()) {
                    std::string dbg = dr_msg.DebugString();
                    auto pos = dbg.find("json:");
                    if (pos != std::string::npos) {
                        jstr = dbg.substr(pos + 5);
                    }
                }

                // std::cerr << "[client] extracted JSON string: " << jstr << "\n";

                std::vector<BBox> boxes;
                if (!jstr.empty()) {
                    boxes = parse_bboxes_from_json(jstr);
                } else {
                    // as last resort, try parse whole ServerMessage DebugString for bracket arrays
                    boxes = parse_bboxes_from_json(sm.DebugString());
                }

                // std::cerr << "[client] parsed " << boxes.size() << " bboxes\n";
                // for (const auto &b : boxes) {
                //     // std::cerr << "  box: x=" << b.x << " y=" << b.y << " w=" << b.w << " h=" << b.h
                //     //           << " score=" << b.score << " label=" << b.label << "\n";
                // }

                if (!boxes.empty()) {
                    Detection det;
                    det.boxes = std::move(boxes);
                    det.timestamp_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
                    {
                        std::lock_guard<std::mutex> lk(det_mtx_);
                        det_queue_.push_back(std::move(det));
                    }
                    det_cv_.notify_one();
                } else {
                    std::cerr << "[client] no boxes parsed from detection_result\n";
                }
            }

            // camera_frame 수신 처리: image_data는 JPEG 바이트(예상)
            if (sm.has_camera_frame()) {
                const auto &cf = sm.camera_frame();
                const std::string &imgdata = cf.image_data();
                if (!imgdata.empty()) {
                    std::vector<uchar> buf(imgdata.begin(), imgdata.end());
                    cv::Mat img = cv::imdecode(buf, cv::IMREAD_COLOR);
                    if (!img.empty()) {
                        std::lock_guard<std::mutex> lk(frame_mtx_);
                        frame_queue_.push_back(img);
                        if (frame_queue_.size() > 4) frame_queue_.erase(frame_queue_.begin()); // bounded
                    } else {
                        std::cerr << "[client] camera_frame imdecode failed\n";
                    }
                }
            }

            // handle config_response terminate ack
            if (sm.has_config_response()) {
                const auto& cr = sm.config_response();
                if (cr.message() == "terminate_ack") {
                    std::cerr << "[client] Received terminate_ack from server -> raising SIGTERM locally\n";
                    ::raise(SIGTERM);
                    break;
                }
            }
        }
        std::cerr << "[client] reader_thread exiting\n";
    }

    bool Send(const std::string& request_data) {
        if (!running_.load()) return false;
        data_types::Command cmd;
        if (request_data == "start_streaming") {
            data_types::ControlAction ca;
            ca.set_action(data_types::ControlAction::START_STREAMING);
            cmd.mutable_control_action()->CopyFrom(ca);
        } else if (request_data == "stop_streaming") {
            data_types::ControlAction ca;
            ca.set_action(data_types::ControlAction::STOP_STREAMING);
            cmd.mutable_control_action()->CopyFrom(ca);
        } else if (request_data == "reboot" || request_data == "32") {
            data_types::ControlAction ca;
            ca.set_action(data_types::ControlAction::REBOOT);
            cmd.mutable_control_action()->CopyFrom(ca);
        } else if (request_data == "wakeup") {
            // One-shot WakeUp RPC:
            // target: env WAKEUP_TARGET or fallback to this client's channel target
            const char* wt = std::getenv("WAKEUP_TARGET");
            std::string wake_target = wt && *wt ? std::string(wt) : std::string("192.168.100.59:50050");
            auto wake_chan = grpc::CreateChannel(wake_target, grpc::InsecureChannelCredentials());
            // generated header uses namespace 'wakemeup' and service class WakeUpService with TriggerScript
            auto wake_stub = wakemeup::WakeUpService::NewStub(wake_chan);

            wakemeup::WakeUpRequest wake_req;
            // set fields if needed: wake_req.set_source("client");
            wakemeup::WakeUpResponse wake_resp;
            grpc::ClientContext wake_ctx;
            grpc::Status wake_status = wake_stub->TriggerScript(&wake_ctx, wake_req, &wake_resp);
            if (!wake_status.ok()) {
                std::cerr << "[client] WakeUp RPC failed -> target=" << wake_target
                          << " err=" << wake_status.error_message() << "\n";
            } else {
                std::cerr << "[client] WakeUp RPC success -> target=" << wake_target << "\n";
            }
            // one-shot RPC, do not send Command over stream
            return wake_status.ok();
        } else {
            // 임의 문자열을 DetectionResult로 보냄
            data_types::DetectionResult det;
            det.set_json(request_data);
            cmd.mutable_detection_result()->CopyFrom(det);
        }
        auto hb = cmd.mutable_heartbeat();
        google::protobuf::Timestamp ts;
        ts.set_seconds(static_cast<int64_t>(std::time(nullptr)));
        ts.set_nanos(0);
        hb->mutable_timestamp()->CopyFrom(ts);

        std::lock_guard<std::mutex> lk(writer_mtx_);
        if (!stream_) return false;
        bool ok = stream_->Write(cmd);
        if (!ok) {
            running_.store(false);
            return false;
        }
        return true;
    }

    bool SendFrameInternal(const cv::Mat& frame) {
        if (!running_.load()) return false;
        data_types::Command cmd;
        auto cf = cmd.mutable_camera_frame();
        std::vector<uint8_t> buf;
        cv::imencode(".jpg", frame, buf);
        cf->set_image_data(buf.data(), buf.size());
        cf->set_width(frame.cols);
        cf->set_height(frame.rows);
        cf->set_format("JPEG");
        auto ts = cf->mutable_timestamp();
        auto now = std::chrono::system_clock::now();
        ts->set_seconds(std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());

        std::lock_guard<std::mutex> lk(writer_mtx_);
        if (!stream_) return false;
        bool ok = stream_->Write(cmd);
        if (!ok) {
            std::cerr << "[client] Write(frame) failed\n";
            running_.store(false);
            return false;
        }
        sent_frames_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void Stop() {
        if (!running_.exchange(false)) return;
        if (context_) context_->TryCancel();
        {
            std::lock_guard<std::mutex> lk(writer_mtx_);
            if (stream_) {
                stream_->WritesDone();
                grpc::Status s = stream_->Finish();
                (void)s;
            }
        }
        if (reader_thread_.joinable()) reader_thread_.join();
        stream_.reset();
        context_.reset();
    }

    // Pop all queued detections
    std::vector<Detection> PopDetections() {
        std::vector<Detection> out;
        std::lock_guard<std::mutex> lk(det_mtx_);
        out.swap(det_queue_);
        return out;
    }

    // Pop one remote frame (thread-safe)
    bool PopRemoteFrame(cv::Mat &out) {
        std::lock_guard<std::mutex> lk(frame_mtx_);
        if (frame_queue_.empty()) return false;
        out = frame_queue_.front();
        frame_queue_.erase(frame_queue_.begin());
        return true;
    }

    uint64_t GetSentFrames() const { return sent_frames_.load(std::memory_order_relaxed); }
    uint64_t GetReceivedResults() const { return received_results_.load(std::memory_order_relaxed); }

    // members
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<compute::ComputeService::Stub> stub_;
    std::unique_ptr<grpc::ClientContext> context_;
    std::unique_ptr<grpc::ClientReaderWriter<data_types::Command, data_types::ServerMessage>> stream_;
    std::thread reader_thread_;
    std::atomic<bool> running_;
    std::mutex writer_mtx_;

    std::atomic<uint64_t> sent_frames_;
    std::atomic<uint64_t> received_results_;

    // detection queue
    std::mutex det_mtx_;
    std::condition_variable det_cv_;
    std::vector<Detection> det_queue_;

    // frame queue
    std::mutex frame_mtx_;
    std::vector<cv::Mat> frame_queue_;
};

// --- forwarding implementations ---

GrpcClient::GrpcClient(const std::string& server_address)
  : channel_(grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials())),
    impl_(std::make_unique<Impl>(channel_))
{}

GrpcClient::~GrpcClient() { if (impl_) impl_->Stop(); }

bool GrpcClient::StartStreaming() { return impl_ ? impl_->Start() : false; }
void GrpcClient::StopStreaming() { if (impl_) impl_->Stop(); }
bool GrpcClient::SendRequest(const std::string& request_data) { return impl_ ? impl_->Send(request_data) : false; }
bool GrpcClient::SendFrame(const cv::Mat& frame) { return impl_ ? impl_->SendFrameInternal(frame) : false; }

uint64_t GrpcClient::GetSentFrames() { return impl_ ? impl_->GetSentFrames() : 0; }
uint64_t GrpcClient::GetReceivedResults() { return impl_ ? impl_->GetReceivedResults() : 0; }

std::vector<GrpcClient::Detection> GrpcClient::PopDetections() {
    return impl_ ? impl_->PopDetections() : std::vector<Detection>{};
}
bool GrpcClient::PopRemoteFrame(cv::Mat &out) { return impl_ ? impl_->PopRemoteFrame(out) : false; }