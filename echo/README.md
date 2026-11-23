# echo: CS:APP 11.4 소켓 인터페이스 실습

책 11.4절의 코드를 옮겨 직접 돌려 보는 실습이다. 과제 웹서버 코드와는 별개다.

## 파일과 책의 대응

| 파일 | 책 | 내용 |
| --- | --- | --- |
| `csapp.h`, `csapp.c` | csapp.c 일부, 그림 11.18·11.19 | 오류 래퍼, RIO, `open_clientfd`, `open_listenfd` |
| `hostinfo.c` | 그림 11.17 | `getaddrinfo` → `getnameinfo` 로 도메인의 IPv4 주소를 출력 |
| `echoclient.c` | 그림 11.20 | 표준 입력의 줄을 보내고 돌아온 줄을 출력 |
| `echoserveri.c` | 그림 11.21 | 반복(iterative) echo 서버. 한 번에 클라이언트 하나 |
| `echo.c` | 그림 11.22 | 연결에서 줄을 읽어 그대로 돌려보낸다 |
| `test_echo.sh` | 없음 | 자동 확인 스크립트 |

`csapp.c` 는 책과 동작이 같다. `-Wall -Wextra` 경고를 없애려고 부호 있는/없는 정수 비교 두 곳(`rio_read`, `Rio_writen`)에 캐스트를 넣고, `rio_readlineb` 의 `n` 을 `size_t` 로 바꿨다.

## 빌드와 실행 (Windows 는 Docker 안에서)

저장소 루트에서 PowerShell 로 이미지를 만들고 컨테이너에 들어간다.

```powershell
docker build -t web-server-dev .
docker run --rm -it --name web-dev -v "${PWD}:/work" web-server-dev
```

컨테이너 안에서 빌드하고 서버를 띄운다.

```bash
cd echo
make
./echoserveri 15213
```

다른 PowerShell 창에서 같은 컨테이너에 클라이언트를 띄운다.

```powershell
docker exec -it web-dev bash -c "cd echo && ./echoclient localhost 15213"
```

줄을 입력하면 그대로 돌아오고, 서버 창에는 `Connected to (localhost, 포트)` 와 `server received N bytes` 가 찍힌다. 클라이언트는 Ctrl+D 로 끝낸다. 서버는 Ctrl+C 로 끝낸다.

## 테스트

```bash
make test        # 보통 빌드로 확인
make asan test   # AddressSanitizer·UBSan 빌드로 메모리 오류까지 확인
```

| 확인 항목 | 기준 |
| --- | --- |
| hostinfo | `./hostinfo localhost` 가 `127.0.0.1` 을 출력한다 |
| echo | 여러 줄(빈 줄, 한글 포함)과 MAXLINE 보다 긴 10000 바이트 줄이 그대로 돌아온다 |
| 다음 연결 | 클라이언트 3개를 차례로 받는다. 데이터 없이 연결만 하고 끊은 클라이언트, 한 줄을 주고받은 뒤 SIGKILL 로 죽은 클라이언트 다음에도 연결을 받는다 |
| 서버 생존 | 모든 테스트가 끝날 때까지 서버가 살아 있다 |
| sanitizer | ASan·LSan·UBSan 오류 보고가 없다 |

서버는 무한 루프라 테스트 끝에 SIGTERM 으로 끝내므로 서버 쪽 누수 검사(LSan)는 돌지 않는다. 정상 종료하는 `echoclient` 와 `hostinfo` 는 누수 검사까지 돈다.

## 직접 확인한 동작과 책 코드의 한계

2026-11-21, Docker Ubuntu 24.04 에서 직접 돌려 본 결과다. 일부러 테스트에는 넣지 않았다. 과제 서버에서 처리할 부분이다.

| 상황 | 결과 | 원인 |
| --- | --- | --- |
| 첫 클라이언트가 연결 중일 때 두 번째 클라이언트 접속 | 두 번째는 첫 클라이언트가 닫을 때까지 응답을 못 받고, 닫으면 받는다 | `echo()` 가 리턴해야 다음 `accept` 를 부른다. 두 번째의 `connect` 는 성공한다. 커널이 3-way handshake 를 끝내고 listen 큐(backlog)에 넣어 두기 때문이다 |
| 같은 포트로 서버를 하나 더 띄움 | 두 번째 서버가 `Open_listenfd error: Address already in use` 로 끝난다 | `SO_REUSEADDR` 은 TIME_WAIT 상태의 주소를 다시 쓰게 해 줄 뿐, 이미 listen 중인 포트에 bind 하게 해 주지는 않는다 |
| 서버를 끄고 바로 같은 포트로 다시 띄움 | 바로 성공한다 | `open_listenfd` 의 `SO_REUSEADDR` |
| 클라이언트가 여러 줄을 보내자마자 죽음 | 서버가 `Rio_readlineb error: Connection reset by peer` 를 찍고 끝난다 | 클라이언트가 읽지 않은 데이터를 남기고 닫거나 닫힌 소켓에 echo 가 도착하면 클라이언트 커널이 RST 를 보낸다. 서버의 다음 `read` 가 `ECONNRESET` 으로 실패하고, 대문자 래퍼가 `unix_error` 로 프로세스를 끝낸다. `write` 쪽에서 먼저 걸리면 SIGPIPE 로 죽는다 |
| 마지막 줄에 줄바꿈이 없음 (`printf 'abc' \| ./echoclient ...`) | 클라이언트와 서버가 둘 다 멈춘다 (교착) | 서버의 `rio_readlineb` 는 `'\n'` 이나 EOF 를 기다리고, 클라이언트는 응답 줄을 기다리느라 연결을 닫지 않는다 |

`unix_error` 는 책대로 `exit(0)` 을 부르므로, 오류로 끝나도 종료 상태는 0 이다.
