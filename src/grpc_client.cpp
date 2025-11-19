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
                std::cout << "[client recv] ServerMessage:\n" << sm.DebugString() << std::endl;

                // 서버가 보낸 종료 ACK 감지: 클라이언트도 종료 진행
                if (sm.has_config_response()) {
                    const auto& cr = sm.config_response();
                    if (cr.message() == "terminate_ack") {
                        std::cerr << "[client] Received terminate_ack from server -> raising SIGTERM locally\n";
                        // 로컬 프로세스 종료 플로우 트리거
                        ::raise(SIGTERM);
                        break;
                    }
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
            auto hb = cmd.mutable_heartbeat();
            google::protobuf::Timestamp ts;
            ts.set_seconds(static_cast<int64_t>(std::time(nullptr)));
            ts.set_nanos(0);
            hb->mutable_timestamp()->CopyFrom(ts);
        }

        std::lock_guard<std::mutex> lk(writer_mtx_);
        if (!stream_) return false;
        // Write may fail if server closed stream; detect and clear running_
        bool ok = stream_->Write(cmd);
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
