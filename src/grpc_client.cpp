#include "grpc_client.h"
#include "ComputeService.grpc.pb.h"
#include "data_types.pb.h"
#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/message.h>
#include <google/protobuf/descriptor.h>
#include <iostream>
#include <sstream>
#include <opencv2/opencv.hpp>
#include <chrono>

class GrpcClient::Impl {
public:
    Impl(std::shared_ptr<grpc::Channel> ch)
      : channel_(ch), stub_(compute::ComputeService::NewStub(ch)), running_(false),
        sent_frames_(0), received_results_(0) {}

    ~Impl() { Stop(); }

    bool Start() {
        if (running_.exchange(true)) return true;
        std::cerr << "[client] Start(): waiting for channel connectivity\n";

        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(5);
        if (!channel_->WaitForConnected(deadline)) {
            std::cerr << "[client] channel failed to become READY within 5s, state="
                      << channel_->GetState(true) << "\n";
            running_ = false;
            return false;
        }
        std::cerr << "[client] channel READY\n";

        context_ = std::make_unique<grpc::ClientContext>();
        // 요청(Command) -> 서버, 응답(ServerMessage) <- 서버
        stream_ = stub_->Datastream(context_.get());
        // reader thread: read server->client ServerMessage and print
        reader_thread_ = std::thread([this]() {
            data_types::ServerMessage sm;
            std::cerr << "[client] reader_thread started\n";
            while (stream_->Read(&sm)) {
                // 기본 Debug 출력 (디버깅용)
                std::cout << "[client recv] ServerMessage Debug:\n" << sm.DebugString() << std::endl;

                // 1) ConfigResponse(종료 ACK) 처리
                if (sm.has_config_response()) {
                    const auto& cr = sm.config_response();
                    if (cr.message() == "terminate_ack") {
                        std::cerr << "[client] Received terminate_ack from server -> raising SIGTERM locally\n";
                        ::raise(SIGTERM);
                        break;
                    } else {
                        std::cerr << "[client] ConfigResponse: success=" << cr.success()
                                  << " msg=" << cr.message() << "\n";
                    }
                }

                // 2) detection_result 내부에 JSON 문자열이 있을 수 있으므로 reflection으로 안전하게 추출
                if (sm.has_detection_result()) {
                    const google::protobuf::Message& dr_msg = sm.detection_result();
                    const google::protobuf::Descriptor* desc = dr_msg.GetDescriptor();
                    const google::protobuf::Reflection* refl = dr_msg.GetReflection();
                    // 흔히 사용하는 필드명 후보들
                    const char* candidates[] = { "json", "json_str", "json_payload", "payload" };
                    for (const char* fname : candidates) {
                        const auto* fd = desc->FindFieldByName(fname);
                        if (fd && fd->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING) {
                            std::string v = refl->GetString(dr_msg, fd);
                            std::cout << "[client recv] detection_result." << fname << " = " << v << "\n";
                        }
                    }
                    received_results_.fetch_add(1, std::memory_order_relaxed);
                    // optional: 출력
                    std::cout << "[client recv] detection_result received\n";
                }

                // 3) camera_frame나 다른 바이너리/텍스트 필드 간단 출력
                if (sm.has_camera_frame()) {
                    const auto& cf = sm.camera_frame();
                    std::cout << "[client recv] CameraFrame size=" << cf.image_data().size()
                              << " " << cf.width() << "x" << cf.height() << "\n";
                }

                // 4) device_status 등 다른 필드 로그
                if (sm.has_device_status()) {
                    const auto& ds = sm.device_status();
                    std::cout << "[client recv] DeviceStatus id=" << ds.device_id()
                              << " state=" << ds.state() << " fps=" << ds.frame_rate_fps() << "\n";
                }
            }
            std::cerr << "[client] reader_thread exiting (server closed stream or termination)\n";
        });
        return true;
    }

    void Stop() {
        if (!running_.exchange(false)) return;
        // Cancel context first to wake up any blocking Read
        if (context_) {
            context_->TryCancel();
        }

        std::lock_guard<std::mutex> lk(writer_mtx_);
        if (stream_) {
            // signal no more writes from client side
            stream_->WritesDone();
            // Finish will wait for server to close; allow it but not block forever
            grpc::Status s = stream_->Finish();
            (void)s;
        }
        if (reader_thread_.joinable()) reader_thread_.join();
        stream_.reset();
        context_.reset();
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
        } else {
            // 임의 문자열을 DetectionResult로 보냄
            data_types::DetectionResult det;
            det.set_json(request_data); // sender side uses 'json' 필드; 서버도 동일 키를 쓰는지 확인
            cmd.mutable_detection_result()->CopyFrom(det);
        }

        std::lock_guard<std::mutex> lk(writer_mtx_);
        if (!stream_) return false;
        bool ok = stream_->Write(cmd);
        if (!ok) {
            std::cerr << "[client] Write returned false -> server likely closed stream. stopping client\n";
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
            running_.store(false);
            return false;
        }
        sent_frames_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    uint64_t GetSentFrames() { return sent_frames_.load(std::memory_order_relaxed); }
    uint64_t GetReceivedResults() { return received_results_.load(std::memory_order_relaxed); }

private:
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<compute::ComputeService::Stub> stub_;
    std::unique_ptr<grpc::ClientContext> context_;
    std::unique_ptr<grpc::ClientReaderWriter<data_types::Command, data_types::ServerMessage>> stream_;
    std::thread reader_thread_;
    std::atomic<bool> running_;
    std::mutex writer_mtx_;
    std::atomic<uint64_t> sent_frames_;
    std::atomic<uint64_t> received_results_;
};

GrpcClient::GrpcClient(const std::string& server_address)
  : channel_(grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials())),
    impl_(std::make_unique<Impl>(channel_))
{}

GrpcClient::~GrpcClient() {
    if (impl_) impl_->Stop();
}

bool GrpcClient::StartStreaming() {
    return impl_ ? impl_->Start() : false;
}

void GrpcClient::StopStreaming() {
    if (impl_) impl_->Stop();
}

bool GrpcClient::SendRequest(const std::string& request_data) {
    if (impl_) {
        if (!impl_->Start()) return false;
        return impl_->Send(request_data);
    }
    return false;
}

bool GrpcClient::SendFrame(const cv::Mat& frame) {
    return impl_ ? impl_->SendFrameInternal(frame) : false;
}

uint64_t GrpcClient::GetSentFrames() { return impl_ ? impl_->GetSentFrames() : 0; }
uint64_t GrpcClient::GetReceivedResults() { return impl_ ? impl_->GetReceivedResults() : 0; }
