// SIGPIPE 처리용 signal이 들어 있는 헤더
#include <signal.h>
// getaddrinfo, getnameinfo, gai_strerror가 들어 있는 헤더
#include <netdb.h>
// printf, fgets, fputs가 들어 있는 헤더
#include <stdio.h>
// strlen이 들어 있는 헤더
#include <string.h>
// write, close가 들어 있는 헤더
#include <unistd.h>
// socket, bind, listen, accept, connect, shutdown이 들어 있는 헤더
#include <sys/socket.h>

// 인자가 포트 하나면 서버, 호스트와 포트 둘이면 클라이언트로 동작한다
int main(int argc, char **argv)
{
    // 인자가 하나면 서버다
    int server = argc == 2;
    // TCP 연결용 주소 조건 (서버는 내 주소로 열기용)
    struct addrinfo hints = { .ai_socktype = SOCK_STREAM, .ai_flags = AI_PASSIVE }, *p;
    // fd = 소켓 번호, n = 줄의 바이트 수, reuse = 포트 재사용 옵션 값, err = 주소 조회 에러 번호
    int fd = -1, n, reuse = 1, err;
    // 한 줄을 담을 칸
    char buf[8192];
    // 소켓을 줄 단위로 읽기 위한 FILE
    FILE *in;

    // 인자 개수가 맞지 않으면 사용법을 알려 주고 끝낸다
    if (argc != 2 && argc != 3)
        return fprintf(stderr, "usage: %s [host] <port>\n", argv[0]), 1;
    // 주소 후보를 구하고, 실패하면 이유를 알려 주고 끝낸다
    if ((err = getaddrinfo(server ? NULL : argv[1], argv[argc - 1], &hints, &p)))
        return fprintf(stderr, "%s\n", gai_strerror(err)), 1;
    // 손님이 먼저 끊은 소켓에 쓸 때 서버가 죽지 않고 write가 -1을 돌려주게 한다
    signal(SIGPIPE, SIG_IGN);

    // 주소 후보를 하나씩 시도해서 성공하는 첫 번째를 쓴다
    for (; p; p = p->ai_next) {
        // 소켓을 만들고, 실패하면 다음 후보로 간다
        if ((fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0)
            continue;
        // 서버를 껐다 켜도 같은 포트를 바로 쓸 수 있게 한다
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
        // 서버는 bind, 클라이언트는 connect를 해서 성공하면(0) 반복을 끝낸다
        if (!(server ? bind(fd, p->ai_addr, p->ai_addrlen) : connect(fd, p->ai_addr, p->ai_addrlen)))
            break;
        // 실패했으면 소켓을 닫고 다음 후보로 간다
        close(fd), fd = -1;
    }
    // 소켓을 못 만들었거나 서버의 listen이 실패하면 에러를 출력하고 끝낸다
    if (fd < 0 || (server && listen(fd, 1024)))
        return perror("echo"), 1;

    // 클라이언트라면
    if (!server) {
        // 소켓을 줄 단위로 읽을 수 있게 FILE로 감싼다
        in = fdopen(fd, "r");
        // 키보드에서 한 줄 읽는다. Ctrl+D를 누르면 NULL이 나와서 끝난다
        while (fgets(buf, sizeof buf, stdin)) {
            // 읽은 줄의 바이트 수를 센다
            n = strlen(buf);
            // 읽은 줄을 서버로 보낸다. 실패하면 끝낸다
            if (write(fd, buf, n) < 0)
                break;
            // 개행 없이 입력이 끝났다면, 서버가 답을 보낼 수 있게 보내기를 닫는다
            if (feof(stdin))
                shutdown(fd, SHUT_WR);
            // 서버가 돌려준 줄을 받는다. 서버가 끊겼으면 반복을 끝낸다
            if (!fgets(buf, sizeof buf, in))
                break;
            // 받은 줄을 화면에 출력한다
            fputs(buf, stdout);
        }
        // 클라이언트는 여기서 끝난다
        return 0;
    }

    // 서버 소켓 번호를 출력한다
    printf("listenfd = %d\n", fd);
    // 손님을 계속 받기 위해 무한 반복한다
    for (;;) {
        // 손님 주소를 담을 상자 (IPv4, IPv6 모두 들어가는 크기)
        struct sockaddr_storage addr;
        // accept가 실제 주소 길이로 덮어쓰므로 매번 상자 크기로 다시 넣는다
        socklen_t len = sizeof addr;
        // 손님의 호스트 이름과 포트를 글자로 담을 칸
        char host[256], port[32];
        // 손님이 올 때까지 멈춰 있다가, 오면 손님 전용 번호를 받는다
        int conn = accept(fd, (void *)&addr, &len);
        // 실패하면 다음 손님을 기다린다
        if (conn < 0)
            continue;
        // 손님의 주소를 호스트 이름과 포트 글자로 바꿔서 출력한다
        getnameinfo((void *)&addr, len, host, sizeof host, port, sizeof port, 0);
        printf("Connected to (%s, %s), connfd = %d\n", host, port, conn);
        // 손님 소켓을 줄 단위로 읽을 수 있게 FILE로 감싼다
        in = fdopen(conn, "r");
        // 손님이 끊을 때까지 한 줄씩 읽는다
        while (fgets(buf, sizeof buf, in)) {
            // 받은 바이트 수를 출력한다
            printf("server received %d bytes\n", n = strlen(buf));
            // 받은 줄을 그대로 돌려보낸다. 손님이 먼저 끊었으면 이 손님은 끝낸다
            if (write(conn, buf, n) < 0)
                break;
        }
        // 이 손님 소켓을 닫는다
        fclose(in);
    }
}
