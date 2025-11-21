#pragma once
#include <grpcpp/grpcpp.h>
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>

namespace cv { class Mat; }

class GrpcClient {
public:
    explicit GrpcClient(const std::string& server_address);
    ~GrpcClient();

    bool StartStreaming();
    void StopStreaming();
    bool SendRequest(const std::string& request_data);
    bool SendFrame(const cv::Mat& frame);

    // performance counters
    uint64_t GetSentFrames();
    uint64_t GetReceivedResults();

private:
    std::shared_ptr<grpc::Channel> channel_;
    class Impl;
    std::unique_ptr<Impl> impl_;
};