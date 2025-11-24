#pragma once
#include <grpcpp/grpcpp.h>
#include "ComputeService.grpc.pb.h"
#include "data_types.pb.h"
#include <mutex>
#include <thread>
#include <atomic>

class ComputeServiceImpl final : public compute::ComputeService::Service {
public:
    grpc::Status Datastream(::grpc::ServerContext* context,
                            ::grpc::ServerReaderWriter<data_types::ServerMessage, data_types::Command>* stream) override;
private:
    // sender thread 관련은 각 RPC 인스턴스별로 로컬 멤버로 관리 (service 인스턴스는 stateless)
};