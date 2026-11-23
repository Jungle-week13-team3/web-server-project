#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net.h"

#define BUF_SIZE 4096

/* 연결 하나를 맡아, 받은 바이트를 그대로 돌려보낸다. 클라이언트가 연결을 닫으면(EOF) 돌아온다. */
static void handle_connection(int connfd)
{
    char buf[BUF_SIZE];
    for (;;) {
        /* echo는 "온 만큼 바로" 돌려줘야 해서 read_all이 아니라 read를 쓴다.
         * read_all은 4096바이트가 다 찰 때까지 기다리기 때문이다. */
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

    /* getaddrinfo는 포트를 문자열로 받으므로 검사한 argv[1]을 그대로 넘긴다 */
    int listenfd = open_listenfd(argv[1]);
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

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cliaddr.sin_addr, ip, sizeof(ip));
        printf("connected: %s:%d\n", ip, ntohs(cliaddr.sin_port));
        fflush(stdout);

        handle_connection(connfd);
        close(connfd);
        printf("closed: %s:%d\n", ip, ntohs(cliaddr.sin_port));
        fflush(stdout);
    }
}
