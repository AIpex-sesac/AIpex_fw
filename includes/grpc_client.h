#pragma once
#include <grpcpp/grpcpp.h>
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

namespace cv { class Mat; }
class GrpcClient {
public:
    explicit GrpcClient(const std::string& server_address);
    ~GrpcClient();

    bool StartStreaming();
    void StopStreaming();
    bool SendRequest(const std::string& request_data);
    bool SendFrame(const cv::Mat& frame);

    // perf counters
    uint64_t GetSentFrames();
    uint64_t GetReceivedResults();

    // Detection structs returned to main for drawing
    struct BBox {
        // normalized coords (0..1) or absolute pixels depending on sender;
        // use floats and convert in main before drawing
        float x; // normalized [0..1] expected
        float y;
        float w;
        float h;
        float score{0.0f};
        std::string label;
    };
    struct Detection {
        std::vector<BBox> boxes;
        uint64_t timestamp_ms{0};
    };

    // pop all pending detection messages (thread-safe)
    std::vector<Detection> PopDetections();
    // Pop one remote frame (thread-safe). returns true and fills out if a frame available.
    bool PopRemoteFrame(cv::Mat &out);

private:
    std::shared_ptr<grpc::Channel> channel_;
    class Impl;
    std::unique_ptr<Impl> impl_;
};