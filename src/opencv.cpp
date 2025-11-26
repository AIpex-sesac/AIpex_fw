#include "opencv.h"

void create_display_window(const std::string &window_name, int width, int height) {
    cv::namedWindow(window_name, cv::WINDOW_AUTOSIZE);
    cv::resizeWindow(window_name, width, height);
}

void show_frame_in_window(const std::string &window_name, const cv::Mat &frame) {
    cv::imshow(window_name, frame);
    cv::waitKey(1); // 1ms 대기하여 화면 갱신
}

void close_display_window(const std::string &window_name) {
    cv::destroyWindow(window_name);
}