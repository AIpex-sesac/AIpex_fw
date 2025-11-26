// OpenCV를 통해 디스플레이에 화면 출력하는 기능을 제공하는 헤더 파일
#pragma once
#include <opencv2/opencv.hpp>
#include <string>

// 디스플레이 윈도우 생성 및 설정
void create_display_window(const std::string &window_name, int width, int height);

// 프레임을 디스플레이 윈도우에 출력
void show_frame_in_window(const std::string &window_name, const cv::Mat &frame);

// 디스플레이 윈도우 닫기
void close_display_window(const std::string &window_name);