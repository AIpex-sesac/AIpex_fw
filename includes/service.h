#pragma once
#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/codegen/service_type.h>

// 빈 서비스: proto-generated 서비스가 준비되기 전까지 서버가 시작되게 함
class EmptyService : public grpc::Service {
public:
    // intentionally empty
};