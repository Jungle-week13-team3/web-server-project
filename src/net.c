#include "net.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BACKLOG 128 /* accept 전까지 줄 세워 둘 연결 요청 수 */

/* 포트 문자열을 받아, 연결 요청을 기다리는 듣기 소켓을 만들고 그 fd를 돌려준다. 실패하면 -1. */
int open_listenfd(const char *port)
{
    struct addrinfo hints, *list, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;       /* IPv4만 쓴다. main이 클라이언트 주소를 IPv4로 출력한다 */
    hints.ai_socktype = SOCK_STREAM; /* TCP */
    hints.ai_flags = AI_PASSIVE      /* 호스트를 비우면 "이 컴퓨터의 모든 IP"로 받는다 */
                   | AI_NUMERICSERV; /* port는 "http" 같은 이름이 아니라 숫자다 */

    int rc = getaddrinfo(NULL, port, &hints, &list);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        return -1;
    }

    /* getaddrinfo는 후보 주소를 여러 개 줄 수 있다. bind에 성공하는 첫 주소를 쓴다 */
    int listenfd = -1;
    for (p = list; p != NULL; p = p->ai_next) {
        listenfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (listenfd < 0)
            continue;

        /* 서버를 껐다가 바로 다시 켜도 bind가 "Address already in use"로 실패하지 않게 한다 */
        int opt = 1;
        if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == 0 &&
            bind(listenfd, p->ai_addr, p->ai_addrlen) == 0)
            break; /* 성공 */

        perror("bind");
        close(listenfd); /* 실패한 소켓은 닫고 다음 후보로 */
        listenfd = -1;
    }
    freeaddrinfo(list); /* 성공하든 실패하든 주소 목록은 해제한다 */

    if (listenfd < 0)
        return -1;
    if (listen(listenfd, BACKLOG) < 0) {
        perror("listen");
        close(listenfd);
        return -1;
    }
    return listenfd;
}

/* host:port 서버에 TCP로 접속하고, 연결된 소켓 fd를 돌려준다. 실패하면 -1. (8단계 프록시용) */
int open_clientfd(const char *host, const char *port)
{
    struct addrinfo hints, *list, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;     /* IPv4든 IPv6든 상대가 가진 주소로 붙는다 */
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV | AI_ADDRCONFIG; /* 이 컴퓨터가 쓸 수 있는 주소 종류만 받는다 */

    int rc = getaddrinfo(host, port, &hints, &list);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo %s:%s: %s\n", host, port, gai_strerror(rc));
        return -1;
    }

    /* 후보 주소를 차례로 시도해서 connect에 성공하는 첫 주소를 쓴다 */
    int clientfd = -1;
    for (p = list; p != NULL; p = p->ai_next) {
        clientfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (clientfd < 0)
            continue;
        if (connect(clientfd, p->ai_addr, p->ai_addrlen) == 0)
            break; /* 성공 */
        close(clientfd);
        clientfd = -1;
    }
    freeaddrinfo(list);
    return clientfd;
}

/* fd에서 len바이트를 다 읽을 때까지 반복해서 buf에 채운다.
 * 읽은 바이트 수를 돌려준다. 중간에 상대가 닫으면(EOF) len보다 작을 수 있다. 오류면 -1. */
ssize_t read_all(int fd, void *buf, size_t len)
{
    char *p = buf;
    size_t left = len;
    while (left > 0) {
        ssize_t n = read(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) /* 시그널 때문에 잠깐 끊긴 것이면 다시 읽는다 */
                continue;
            return -1;
        }
        if (n == 0) /* EOF: 더 올 데이터가 없다 */
            break;
        p += n;
        left -= (size_t)n;
    }
    return (ssize_t)(len - left);
}

/* buf의 len바이트를 fd에 끝까지 쓴다. write는 한 번에 다 못 쓸 수 있어서 반복한다. 성공 0, 실패 -1. */
int write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}
