#pragma once
#include "app_comm.grpc.pb.h"
#include <grpcpp/grpcpp.h>

class AppCommServiceImpl final : public app_communication::AppCommService::Service {
public:
    grpc::Status SendJSON(::grpc::ServerContext* context,
                          const app_communication::JSONRequest* request,
                          app_communication::JSONResponse* response) override;

    grpc::Status ReceiveJSON(::grpc::ServerContext* context,
                             const app_communication::JSONRequest* request,
                             app_communication::JSONResponse* response) override;
};