#pragma once
#include <grpcpp/grpcpp.h>
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <opencv2/core/mat.hpp>

class GrpcClient {
public:
    explicit GrpcClient(const std::string& server_address);
    ~GrpcClient();

    // 시작/정지 및 메시지 전송
    bool StartStreaming();
    void StopStreaming();
    bool SendRequest(const std::string& request_data);
    // send encoded frame (mat will be JPEG-encoded inside)
    bool SendFrame(const cv::Mat& frame);

private:
    std::shared_ptr<grpc::Channel> channel_;
    class Impl;
    std::unique_ptr<Impl> impl_;
};