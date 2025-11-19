### AIPEX C++ Firmware

이 레포지토리는 AIPEX 연산 보드에 올라갈 펌웨어 코드 저장소입니다. 현재 구현된 기능은 다음과 같습니다.

- gRPC 양방향 스트리밍을 통한 서버-디바이스 통신
- 절전 모드 지원(dummy 구현)

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
- export "GRPC_TARGET"=(대상 ip):50051
- (선택) export "GRPC_PORT"=(로컬 ip):(원하는 포트)
4. 실행
    ```./AipexFW```

### 코드 구조
- `includes` : 헤더 파일
- `src` : 소스 파일
- `protos` : Protocol Buffers 정의 파일


