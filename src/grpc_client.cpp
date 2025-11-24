#include "grpc_client.h"
#include "ComputeService.grpc.pb.h"
#include "data_types.pb.h"
#include <google/protobuf/timestamp.pb.h>
#include <iostream>
#include <sstream>
#include <csignal> // added

class GrpcClient::Impl {
public:
    Impl(std::shared_ptr<grpc::Channel> ch)
      : channel_(ch), stub_(compute::ComputeService::NewStub(ch)), running_(false) {}

    ~Impl() { Stop(); }

    bool Start() {
        if (running_.exchange(true)) return true;
        std::cerr << "[client] Start(): waiting for channel connectivity\n";

        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(5);
        if (!channel_->WaitForConnected(deadline)) {
            std::cerr << "[client] channel failed to become READY within 5s, state="
                      << channel_->GetState(true) << "\n";
            running_.store(false);
            return false;
        }
        std::cerr << "[client] channel READY\n";

        context_ = std::make_unique<grpc::ClientContext>();
        stream_ = stub_->Datastream(context_.get());
        if (!stream_) {
            std::cerr << "[client] Failed to create stream_ (stub_->Datastream returned null)\n";
            running_.store(false);
            return false;
        }
        // reader thread: read server->client ServerMessage and print
        reader_thread_ = std::thread([this]() {
            data_types::ServerMessage sm;
            while (running_.load()) {
                if (!stream_) break;
                bool ok = stream_->Read(&sm);
                if (!ok) {
                    std::cerr << "[client] server closed stream or read failed\n";
                    break;
                }
                std::cerr << "[client] received ServerMessage\n";
            }
            std::cerr << "[client] reader thread exiting\n";
        });
        return true;
    }

    void Stop() {
        if (!running_.exchange(false)) return;
        std::cerr << "[client] Stop(): shutting down\n";
        // Cancel context first to wake blocking reads/writes
        if (context_) context_->TryCancel();

        {
            std::lock_guard<std::mutex> lk(writer_mtx_);
            if (stream_) {
                stream_->WritesDone();
                grpc::Status status = stream_->Finish();
                if (!status.ok()) {
                    std::cerr << "[client] stream finish error: " << status.error_message() << "\n";
                }
            }
        }

        if (reader_thread_.joinable()) reader_thread_.join();
        stream_.reset();
        context_.reset();
    }

    bool Send(const std::string& request_data) {
        if (!running_.load()) {
            std::cerr << "[client send] rejected: not running\n";
            return false;
        }
        data_types::Command cmd;
        if (request_data == "start_streaming") {
            data_types::ControlAction ca;
            ca.set_action(data_types::ControlAction::START_STREAMING);
            cmd.mutable_control_action()->CopyFrom(ca);
        } else if (request_data == "stop_streaming") {
            data_types::ControlAction ca;
            ca.set_action(data_types::ControlAction::STOP_STREAMING);
            cmd.mutable_control_action()->CopyFrom(ca);
        } else if (request_data == "heartbeat") {
            data_types::Heartbeat hb;
            auto now = std::chrono::system_clock::now();
            google::protobuf::Timestamp ts;
            ts.set_seconds(std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
            cmd.mutable_heartbeat()->CopyFrom(hb);
            cmd.mutable_heartbeat()->mutable_timestamp()->CopyFrom(ts);
        } else {
            // 임의 문자열을 DetectionResult로 보냄
            data_types::DetectionResult det;
            det.set_json(request_data);
            cmd.mutable_detection_result()->CopyFrom(det);
        }

        // // 디버그 로그: payload preview + full protobuf content
        // std::cerr << "[client send] payload_len=" << request_data.size()
        //           << " preview=\"" << (request_data.size() > 120 ? request_data.substr(0,120) + "..." : request_data) << "\"\n";
        // std::cerr << "[client send] cmd proto:\n" << cmd.DebugString() << "\n";

        std::lock_guard<std::mutex> lk(writer_mtx_);
        if (!stream_) {
            std::cerr << "[client] Send failed: stream_ is null\n";
            return false;
        }
        bool ok = stream_->Write(cmd);
        std::cerr << "[client] Write returned " << ok << "\n";
        if (!ok) {
            std::cerr << "[client] Write returned false -> server likely closed stream. stopping client\n";
            running_.store(false);
            return false;
        }
        return true;
    }

private:
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<compute::ComputeService::Stub> stub_;
    std::unique_ptr<grpc::ClientContext> context_;
    // 변경: 템플릿 인자 순서(Request, Response)
    std::unique_ptr<grpc::ClientReaderWriter<data_types::Command, data_types::ServerMessage>> stream_;
    std::thread reader_thread_;
    std::atomic<bool> running_;
    std::mutex writer_mtx_;
};

GrpcClient::GrpcClient(const std::string& server_address)
  : channel_(grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials())),
    impl_(std::make_unique<Impl>(channel_))
{
}

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
