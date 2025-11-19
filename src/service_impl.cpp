#include "service_impl.h"
#include <iostream>
#include <csignal>
#include <chrono>

grpc::Status ComputeServiceImpl::Datastream(::grpc::ServerContext* context,
                                            ::grpc::ServerReaderWriter<data_types::ServerMessage, data_types::Command>* stream) {
    std::mutex write_mtx;
    std::atomic<bool> running{true};
    std::thread sender;

    auto send_device_status = [&]() {
        while (running.load()) {
            // 종료/취소 체크
            if (context->IsCancelled()) {
                std::cerr << "[service sender] context cancelled, exiting sender\n";
                break;
            }

            data_types::ServerMessage sm;
            auto ds = sm.mutable_device_status();
            ds->set_device_id("device_001");
            ds->set_state(data_types::DeviceStatus::GRPC_READY);
            ds->set_frame_rate_fps(30.0f);
            ds->set_cpu_temperature_c(45.0f);
            ds->set_processing_latency_ms(10);

            {
                std::lock_guard<std::mutex> lk(write_mtx);
                bool ok = stream->Write(sm);
                if (!ok) {
                    std::cerr << "[service sender] Write returned false (client closed). Stopping sender\n";
                    break;
                }
            }
            // 주기 (예시 1초)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    };

    // sender 스레드 시작
    sender = std::thread(send_device_status);

    data_types::Command cmd;
    std::cerr << "[service] Datastream handler entered\n";
    while (running.load() && stream->Read(&cmd)) {
        // 읽은 메시지 출력
        std::cout << "[service recv] Command received:\n" << cmd.DebugString() << std::endl;

        // ControlAction 처리
        if (cmd.has_control_action()) {
            const auto& ca = cmd.control_action();
            std::cout << "[service recv] ControlAction action=" << ca.action() << std::endl;
            if (ca.action() == data_types::ControlAction::REBOOT ||
                ca.action() == data_types::ControlAction::STOP_STREAMING) {
                std::cerr << "[service] ControlAction -> sending ack and raising SIGTERM\n";
                data_types::ConfigResponse resp;
                resp.set_success(true);
                resp.set_message("terminate_ack");
                data_types::ServerMessage sm;
                sm.mutable_config_response()->CopyFrom(resp);
                {
                    std::lock_guard<std::mutex> lk(write_mtx);
                    stream->Write(sm);
                }
                // 프로세스 내에서 우아한 종료를 하려면 raise 또는 context 취소를 사용
                ::raise(SIGTERM);
                break;
            }
        }

        // Heartbeat 처리 예시 (로그만)
        if (cmd.has_heartbeat()) {
            const auto& hb = cmd.heartbeat();
            if (hb.has_timestamp()) {
                std::cerr << "[service] Heartbeat ts=" << hb.timestamp().seconds() << "\n";
            } else {
                std::cerr << "[service] Heartbeat(no timestamp)\n";
            }
        }

        // 만약 서버가 즉시 응답해야하면 여기서 Write
        if (cmd.has_config_request()) {
            data_types::ConfigResponse resp;
            resp.set_success(true);
            resp.set_message("config_saved");
            data_types::ServerMessage sm;
            sm.mutable_config_response()->CopyFrom(resp);
            {
                std::lock_guard<std::mutex> lk(write_mtx);
                if (!stream->Write(sm)) {
                    std::cerr << "[service] Write failed while responding to config_request\n";
                    break;
                }
            }
        }

        // 취소 확인
        if (context->IsCancelled()) {
            std::cerr << "[service] context cancelled detected in read loop\n";
            break;
        }
    }

    // 정리
    running.store(false);
    if (sender.joinable()) sender.join();
    std::cerr << "[service] Datastream RPC exiting\n";
    return grpc::Status::OK;
}