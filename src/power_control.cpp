#include "power_control.h"
#include <iostream>
#include <thread>
#include <chrono>

// libgpiod 사용 가능하면 그쪽으로, 없으면 sysfs 쪽 주석/대체 사용.
// 보드 설계에 맞게 GPIO 칩/라인 변경 필요.
static const char* GPIO_CHIP = "/dev/gpiochip0";
static const unsigned int HAILO_PWR_LINE = 17;

void PrepareForSuspend() {
    std::cerr << "[power] PrepareForSuspend: stopping inference and cutting Hailo power\n";
    // 1) Hailo SDK shutdown 호출(적절한 위치에서 호출)
    // 2) 보드 전원 게이트 끄기 (GPIO toggle) - 실제 구현은 libgpiod 사용 권장
    // 자리표시자: 외부 스크립트로 제어할 경우 system() 호출 등으로 대체 가능
}

void RecoverFromResume() {
    std::cerr << "[power] RecoverFromResume: enabling Hailo power and re-init\n";
    // 1) 보드 전원 게이트 켜기
    // 2) 드라이버/SDK 재초기화 (약간의 딜레이 필요)
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}