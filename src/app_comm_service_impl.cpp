#include "app_comm_service_impl.h"
#include <iostream>
#include <string>
// forwarding client
#include "grpc_client.h"
// app_comm proto client
#include "app_comm.grpc.pb.h"
#include <grpcpp/grpcpp.h>

using namespace app_communication;

grpc::Status AppCommServiceImpl::SendJSON(::grpc::ServerContext* /*ctx*/,
                                          const JSONRequest* request,
                                          JSONResponse* response) {
    const std::string &j = request->json_payload();
    std::cerr << "[appcomm] Received SendJSON len=" << j.size()
              << " preview=\"" << (j.size()>120 ? j.substr(0,120) + "..." : j) << "\"\n";

    // Forward to remote AppCommService (unary RPC) if AIPEX_FORWARD_TARGET set
    if (const char* target = std::getenv("AIPEX_FORWARD_TARGET")) {
        std::string fw_target(target);
        try {
            auto channel = grpc::CreateChannel(fw_target, grpc::InsecureChannelCredentials());
            auto stub = app_communication::AppCommService::NewStub(channel);

            app_communication::JSONRequest req;
            req.set_json_payload(j);
            app_communication::JSONResponse resp;
            grpc::ClientContext ctx;
            ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));

            grpc::Status status = stub->SendJSON(&ctx, req, &resp);
            if (!status.ok()) {
                std::cerr << "[appcomm] forward SendJSON failed -> " << fw_target
                          << " : " << status.error_message() << "\n";
            } else {
                std::cerr << "[appcomm] forward SendJSON OK -> " << fw_target
                          << " resp=" << resp.message() << "\n";
            }
        } catch (const std::exception &e) {
            std::cerr << "[appcomm] forward exception: " << e.what() << "\n";
        }
    }

    // TODO: 필요하면 JSON 파싱/검증/큐잉 수행
    response->set_success(true);
    response->set_message("received");
    return grpc::Status::OK;
}

grpc::Status AppCommServiceImpl::ReceiveJSON(::grpc::ServerContext* /*ctx*/,
                                             const JSONRequest* request,
                                             JSONResponse* response) {
    const std::string &j = request->json_payload();
    std::cerr << "[appcomm] Received ReceiveJSON len=" << j.size() << "\n";
    response->set_success(true);
    response->set_message("received");
    return grpc::Status::OK;
}