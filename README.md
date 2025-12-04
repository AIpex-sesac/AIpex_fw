### AIPEX C++ Firmware

이 레포지토리는 AIPEX 연산 보드에 올라갈 펌웨어 코드 저장소입니다. 현재 구현된 기능은 다음과 같습니다.

- gRPC 양방향 스트리밍을 통한 서버-디바이스 통신
- gRPC 단방향 스트리밍을 받아 화면 출력 확인
- 앱으로부터 json 형식의 단발성 통신을 받아 원하는 장치로 forwarding
- wakeup service 구현을 통해 연산 펌웨어가 전원이 들어와 실행될 때 출력 보드로 신호를 보내어 출력 보드 화면 실행

### 빌드 및 실행 방법

1. 의존성 설치

   ```bash
   sudo apt-get update
   sudo apt-get install -y build-essential cmake libgrpc++-dev protobuf-compiler libprotobuf-dev
   ```
2. 레포지토리 클론
3. 빌드

   ```bash
   mkdir build
   cd build
   cmake ..
   make
   ```
3.5 환경변수 설정
- GRPC_TARGET: 특정 서버로부터 신호를 받고 싶을 때 사용
- AIPEX_FORWARD_TARGET: app_comm.proto에 정의된 앱을 위한 통신을 통해 받은 json을 포워딩 할 ip, port 지정
- (테스트용) HAILO_LOWLIGHT_ENHANCE: Object Detection 대신 Low Light Enhancement 결과 이미지를 전송하도록 동작 수정하는 변수, 1일 경우 활성화
      - 해당 기능 실행 시 화면을 출력하여 hailo model zoo에서 제공한 zero_dce_pp.hef inference 처리값을 출력함

4. 실행
    ```
    cd /path/to/repository/build/
    ./AipexFW
    ```

### 코드 구조
- `includes` : 헤더 파일
- `src` : 소스 파일
- ComputeService: Hailo Inference 관련 통신 서비스 선언
- data_types: 위 파일에서 사용되는 Protocol buffer 선언
- wakeup: 출력 보드 동작 제어 관련 Protocol buffer

### 개선 방향
- 사용하지 않는 파일들 정리
      - power_control 부분은 알아봤을 때 hailo power 관련 통제가 어려워 현재 개발에서 배제되어 삭제 가능
      - cpp 통신 테스트로 썼던 greeter.cpp 등
      - config 관련 코드는 현재 네트워크 환경 특성상 상호 mac 주소 저장을 해도 연결하기 위해 어짜피 네트워크 탐색을 거쳐야 하기 때문에 후순위로 미뤄짐
      - hailo 관련 예시 코드를 그대로 가져온 후 다시 수정하는 과정에서 더이상 사용되지 않는 값들이 많음
- cmake link 관련 메모리 문제 해결
      - 현재 cmake와 g++를 통해 컴파일 할 때 마지막 실행파일을 만드는 link 과정에서 라즈베리 파이에서 out of memory 관련 크래시를 발생시킴
      - 위의 사용하지 않는 파일 정리를 통해 더 이상 사용하지 않는 파일들을 정리하고  

