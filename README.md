# web-server

Krafton Jungle SW-AI Lab Week13 web-server project

외부 HTTP 라이브러리 없이 C와 POSIX 소켓 API만으로 만드는 웹 서버다. 최종 목표는 정적 파일 서빙, 동적 요청 처리, Reverse Proxy, 동시 요청 처리까지 구현하는 것이다.

## 현재 상태

| 단계 | 내용 | 상태 |
| --- | --- | --- |
| TCP 소켓 서버 | `socket` → `bind` → `listen` → `accept`, 받은 데이터를 그대로 돌려주는 echo | 완료 |
| HTTP 요청·응답 | 요청 파싱, 응답 생성 | 예정 |
| 정적 파일 | MIME 타입, 404·403·500 | 예정 |
| 동적 요청 | CGI | 예정 |
| Reverse Proxy | 경로 기반 라우팅, Round-Robin | 예정 |
| 동시 처리 | 스레드 풀 | 예정 |

## 개발 환경

Dev Container(Ubuntu 24.04)에서 개발한다. Windows에서는 Docker Desktop이 필요하다.

1. Docker Desktop을 켠다.
2. VS Code에 Dev Containers 확장을 설치한다.
3. 이 폴더를 VS Code로 열고 `Ctrl+Shift+P` → **Dev Containers: Reopen in Container**를 고른다.

컨테이너에는 gcc, make, gdb, valgrind, curl, telnet, ab가 들어 있다.

## 빌드

```bash
make        # ./server 생성
make clean  # 빌드 결과 삭제
```

## 실행

```bash
./server <port>
```

예: `./server 8888`

## echo 동작 확인

터미널 두 개를 연다.

```bash
# 터미널 1: 서버
./server 8888

# 터미널 2: 클라이언트
telnet localhost 8888
```

터미널 2에서 글자를 입력하고 Enter를 누르면 같은 글자가 돌아온다. telnet은 `Ctrl + ]` → `quit`으로 끝낸다.

## 알려진 제한

- **한 번에 연결 하나만 처리한다.** 앞 연결이 끝나야 다음 연결을 받는다. 동시 처리 단계에서 고친다.
- **8080 포트로 테스트하면 응답이 없을 수 있다.** VS Code 포트 포워딩이 8080에 연결을 먼저 맺고 붙잡고 있어서, 한 번에 하나만 처리하는 지금 서버는 다음 연결을 받지 못한다. 지금은 포워딩 목록(8080, 9001, 9002)에 없는 포트로 테스트한다.
- IPv4만 받는다. telnet이 `::1`(IPv6) 접속에 실패한 뒤 `127.0.0.1`로 다시 접속하는 것은 정상이다.

## 폴더 구조

```text
.devcontainer/   개발 컨테이너 설정
src/main.c       서버 소스
Makefile         빌드 규칙
```
