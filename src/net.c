#define _GNU_SOURCE /* SOCK_CLOEXEC: 리눅스 확장. 모든 #include보다 먼저 와야 한다 */
#include "net.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <poll.h>     /* poll: 시간 제한 안에서 읽기 가능해지길 기다림 */
#include <sys/time.h> /* struct timeval: 읽기 타임아웃 */
#include <time.h>
#include <unistd.h>

#define BACKLOG 128 /* accept 전까지 줄 세워 둘 연결 요청 수 */

/* 포트 문자열을 받아, 연결 요청을 기다리는 듣기 소켓을 만들고 그 fd를 돌려준다. 실패하면 -1. */
int open_listen_socket(const char *port)
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
        /* SOCK_CLOEXEC: CGI 프로그램(execve)에 이 소켓을 물려주지 않는다 */
        listenfd = socket(p->ai_family, p->ai_socktype | SOCK_CLOEXEC, p->ai_protocol);
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
int connect_to_server(const char *host, const char *port)
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
        clientfd = socket(p->ai_family, p->ai_socktype | SOCK_CLOEXEC, p->ai_protocol);
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
ssize_t read_exact(int fd, void *buf, size_t len)
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

/* reader가 fd에서 읽도록 준비한다. 버퍼는 비어 있는 상태로 시작한다. 시간 제한은 없음(0). */
void init_line_reader(struct line_reader *reader, int fd)
{
    reader->fd = fd;
    reader->start = 0;
    reader->end = 0;
    reader->deadline = 0;
}

/* 지금부터 seconds초 뒤를 읽기 시간 제한으로 건다. seconds<=0이면 제한을 푼다.
 * 요청 하나를 받기 시작할 때 걸면, 느린 요청이 워커를 영원히 붙잡지 못하고 그 시각에 끊긴다. */
void set_line_reader_deadline(struct line_reader *reader, int seconds)
{
    reader->deadline = seconds > 0 ? time(NULL) + seconds : 0;
}

/* deadline까지 fd가 읽기 가능해질 때까지 기다린다. deadline==0이면 제한 없이 바로 1(블로킹 read에 맡긴다).
 * 준비됨 1, 시간 초과 0, 오류 -1. */
static int wait_readable(int fd, time_t deadline)
{
    if (deadline == 0)
        return 1;
    for (;;) {
        time_t now = time(NULL);
        if (now >= deadline)
            return 0;
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int rc = poll(&p, 1, (int)((deadline - now) * 1000));
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        return rc > 0 ? 1 : 0;
    }
}

/* 한 줄을 읽어 out에 넣는다. 줄 끝의 "\r\n"(또는 "\n")은 빼고, 끝에 '\0'을 붙인다.
 * 줄 길이(0이면 빈 줄)를 돌려준다. 실패하면 READ_LINE_EOF / _ERROR / _TOO_LONG. */
ssize_t read_line(struct line_reader *reader, char *out, size_t cap)
{
    size_t len = 0; /* out에 지금까지 담은 글자 수 */

    for (;;) {
        /* 받아 둔 바이트를 다 꺼냈으면 소켓에서 새로 받아 온다 */
        if (reader->start == reader->end) {
            int ready = wait_readable(reader->fd, reader->deadline);
            if (ready == 0) /* 요청 전체 시간 제한 초과: 느린 요청이다 */
                return READ_LINE_TIMEOUT;
            if (ready < 0)
                return READ_LINE_ERROR;
            ssize_t n = read(reader->fd, reader->buf, sizeof(reader->buf));
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return READ_LINE_ERROR;
            }
            if (n == 0) /* 줄바꿈이 오기 전에 상대가 닫았다 = 불완전한 줄 */
                return READ_LINE_EOF;
            reader->start = 0;
            reader->end = (size_t)n;
        }

        char c = reader->buf[reader->start++];
        if (c == '\n') {
            if (len > 0 && out[len - 1] == '\r') /* HTTP 줄 끝은 "\r\n"이라 '\r'도 뗀다 */
                len--;
            out[len] = '\0';
            return (ssize_t)len;
        }
        if (len + 1 >= cap) /* '\0' 자리까지 남겨야 하므로 +1 */
            return READ_LINE_TOO_LONG;
        out[len++] = c;
    }
}

/* reader에서 정확히 len바이트를 읽어 buf에 채운다. 본문(POST)을 읽을 때 쓴다.
 * 헤더를 읽을 때 본문 앞부분이 이미 reader 버퍼에 들어와 있을 수 있어서, 그것부터 꺼내고 모자란 만큼만 소켓에서 읽는다.
 * 읽은 바이트 수를 돌려준다. 중간에 상대가 닫으면 len보다 작다. 오류면 -1. */
ssize_t read_buffered_exact(struct line_reader *reader, void *buf, size_t len)
{
    size_t buffered = reader->end - reader->start;
    size_t from_buffer = buffered < len ? buffered : len;
    memcpy(buf, reader->buf + reader->start, from_buffer);
    reader->start += from_buffer;

    /* 모자란 만큼 소켓에서 더 읽는다. deadline이 지나면 짧게 반환해서(호출부가 실패 처리) 느린 본문이 워커를 안 붙잡게 한다 */
    size_t got = from_buffer;
    char *p = buf;
    while (got < len) {
        int ready = wait_readable(reader->fd, reader->deadline);
        if (ready <= 0) /* 시간 초과 또는 오류 */
            return (ssize_t)got;
        ssize_t n = read(reader->fd, p + got, len - got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) /* EOF: 상대가 중간에 닫았다 */
            break;
        got += (size_t)n;
    }
    return (ssize_t)got;
}

/* reader에서 최대 cap바이트를 한 번 읽는다. 버퍼에 남은 게 있으면 그것을, 없으면 소켓에서 read 한 번.
 * 크기를 모르는 데이터를 조각조각 옮길 때 쓴다. 읽은 바이트 수, EOF면 0, 오류면 -1. */
ssize_t read_some(struct line_reader *reader, void *buf, size_t cap)
{
    size_t buffered = reader->end - reader->start;
    if (buffered > 0) {
        size_t n = buffered < cap ? buffered : cap;
        memcpy(buf, reader->buf + reader->start, n);
        reader->start += n;
        return (ssize_t)n;
    }
    for (;;) {
        ssize_t n = read(reader->fd, buf, cap);
        if (n < 0 && errno == EINTR)
            continue;
        return n;
    }
}

#define DRAIN_TIMEOUT_SEC 1           /* 남은 입력을 기다리는 read 한 번의 최대 시간 */
#define DRAIN_TOTAL_SEC   2           /* 버리기 전체에 쓰는 최대 시간 */
#define DRAIN_MAX_BYTES   (64 * 1024) /* 버릴 수 있는 최대 바이트 */

/* 응답을 다 보낸 연결을 닫는다. 그냥 close하면 안 읽은 입력이 남았을 때 OS가 RST를 보내서
 * 클라이언트가 응답을 못 읽을 수 있다. 그래서 FIN을 먼저 보내고, 남은 입력을 읽어 버린 뒤 닫는다.
 * 상대가 안 닫아도 서버가 멈추지 않게 시간과 바이트 수에 상한을 둔다. */
void drain_and_close(int fd)
{
    /* 쓰기 방향만 닫는다 = "보낼 건 다 보냈다"(FIN). 읽기는 아직 가능하다 */
    shutdown(fd, SHUT_WR);

    /* read가 DRAIN_TIMEOUT_SEC 넘게 기다리면 -1(EAGAIN)로 돌아오게 한다 */
    struct timeval tv = { .tv_sec = DRAIN_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    time_t deadline = time(NULL) + DRAIN_TOTAL_SEC;
    size_t drained = 0;
    char buf[4096];
    while (drained < DRAIN_MAX_BYTES && time(NULL) < deadline) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) /* 0 = 상대도 닫음(정상), -1 = 타임아웃이나 오류 → 그만 기다린다 */
            break;
        drained += (size_t)n;
    }
    close(fd);
}
