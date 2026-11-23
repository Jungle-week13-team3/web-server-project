#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BACKLOG 128   /* accept 전까지 줄 세워 둘 연결 요청 수 */
#define BUF_SIZE 4096

/* 포트 번호를 받아, 연결 요청을 기다리는 듣기 소켓을 만들고 그 fd를 돌려준다. 실패하면 -1. */
static int open_listen_socket(int port)
{
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        perror("socket");
        return -1;
    }

    /* 서버를 껐다가 바로 다시 켜도 bind가 "Address already in use"로 실패하지 않게 한다 */
    int opt = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(listenfd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY); /* 이 호스트의 어느 IP로 와도 받는다 */
    addr.sin_port = htons((uint16_t)port);    /* 포트는 네트워크 바이트 순서로 넣는다 */

    if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listenfd);
        return -1;
    }
    if (listen(listenfd, BACKLOG) < 0) {
        perror("listen");
        close(listenfd);
        return -1;
    }
    return listenfd;
}

/* buf의 len바이트를 fd에 끝까지 쓴다. write는 한 번에 다 못 쓸 수 있어서 반복한다. 성공 0, 실패 -1. */
static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) /* 시그널 때문에 잠깐 끊긴 것이면 다시 쓴다 */
                continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* 연결 하나를 맡아, 받은 바이트를 그대로 돌려보낸다. 클라이언트가 연결을 닫으면(EOF) 돌아온다. */
static void handle_connection(int connfd)
{
    char buf[BUF_SIZE];
    for (;;) {
        ssize_t n = read(connfd, buf, sizeof(buf));
        if (n == 0) /* EOF: 클라이언트가 연결을 닫았다 */
            return;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("read");
            return;
        }
        if (write_all(connfd, buf, (size_t)n) < 0) {
            perror("write");
            return;
        }
    }
}

/* 포트를 인자로 받아 듣기 소켓을 열고, 연결을 하나씩 순서대로 받아 처리한다(반복 서버). */
int main(int argc, char **argv)
{
    //argc이 2인지 확인
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return 1;
    }

    // 포트번호가 맞는지 확인
    char *end;
    long port = strtol(argv[1], &end, 10);
    if (*end != '\0' || port < 1 || port > 65535) {
        fprintf(stderr, "invalid port: %s\n", argv[1]);
        return 1;
    }


    int listenfd = open_listen_socket((int)port);
    if (listenfd < 0)
        return 1;
    printf("listening on port %ld\n", port);
    fflush(stdout);

    for (;;) {
        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        int connfd = accept(listenfd, (struct sockaddr *)&cliaddr, &clilen);
        if (connfd < 0) {
            if (errno != EINTR)
                perror("accept");
            continue;
        }

        // 클라이언트 주소 출력
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cliaddr.sin_addr, ip, sizeof(ip));
        printf("connected: %s:%d\n", ip, ntohs(cliaddr.sin_port));
        fflush(stdout);

        //에코 출력
        handle_connection(connfd);
        close(connfd);
        printf("closed: %s:%d\n", ip, ntohs(cliaddr.sin_port));
        fflush(stdout);
    }
}