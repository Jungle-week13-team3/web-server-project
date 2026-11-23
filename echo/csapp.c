/*
 * csapp.c - CS:APP 3판 csapp.c 중 11.4절(소켓 인터페이스, echo)에 필요한 부분만 옮긴 것.
 *
 * 책과 다른 점: -Wall -Wextra 경고를 없애려고 부호 있는/없는 정수 비교에 캐스트를 넣었다.
 * 동작은 책 코드와 같다.
 */
#include "csapp.h"

/**************************
 * 오류 출력 함수
 **************************/

/* errno 를 쓰는 Unix 스타일 오류 */
void unix_error(char *msg)
{
    fprintf(stderr, "%s: %s\n", msg, strerror(errno));
    exit(0);
}

/* getaddrinfo·getnameinfo 처럼 오류 코드를 리턴하는 함수의 오류 */
void gai_error(int code, char *msg)
{
    fprintf(stderr, "%s: %s\n", msg, gai_strerror(code));
    exit(0);
}

/* 응용 프로그램 오류 */
void app_error(char *msg)
{
    fprintf(stderr, "%s\n", msg);
    exit(0);
}

/**************************
 * Unix I/O 래퍼
 **************************/

void Close(int fd)
{
    int rc;

    if ((rc = close(fd)) < 0)
        unix_error("Close error");
}

/**************************
 * 소켓 인터페이스 래퍼
 **************************/

int Accept(int s, struct sockaddr *addr, socklen_t *addrlen)
{
    int rc;

    if ((rc = accept(s, addr, addrlen)) < 0)
        unix_error("Accept error");
    return rc;
}

void Setsockopt(int s, int level, int optname, const void *optval, int optlen)
{
    int rc;

    if ((rc = setsockopt(s, level, optname, optval, optlen)) < 0)
        unix_error("Setsockopt error");
}

/************************************
 * 프로토콜에 무관한 주소 변환 래퍼
 ************************************/

void Getaddrinfo(const char *node, const char *service,
                 const struct addrinfo *hints, struct addrinfo **res)
{
    int rc;

    if ((rc = getaddrinfo(node, service, hints, res)) != 0)
        gai_error(rc, "Getaddrinfo error");
}

void Getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host,
                 size_t hostlen, char *serv, size_t servlen, int flags)
{
    int rc;

    if ((rc = getnameinfo(sa, salen, host, hostlen, serv, servlen, flags)) != 0)
        gai_error(rc, "Getnameinfo error");
}

void Freeaddrinfo(struct addrinfo *res)
{
    freeaddrinfo(res);
}

/**************************
 * 표준 I/O 래퍼
 **************************/

char *Fgets(char *ptr, int n, FILE *stream)
{
    char *rptr;

    if (((rptr = fgets(ptr, n, stream)) == NULL) && ferror(stream))
        app_error("Fgets error");
    return rptr;
}

void Fputs(const char *ptr, FILE *stream)
{
    if (fputs(ptr, stream) == EOF)
        unix_error("Fputs error");
}

/****************************************
 * RIO 패키지 (10.5절): 짧은 읽기·쓰기를 견디는 I/O
 ****************************************/

/*
 * rio_writen - n 바이트를 모두 쓸 때까지 write 를 반복한다.
 * 소켓에 대한 write 는 요청보다 적게 쓰고 리턴할 수 있다(짧은 쓰기).
 */
ssize_t rio_writen(int fd, void *usrbuf, size_t n)
{
    size_t nleft = n;
    ssize_t nwritten;
    char *bufp = usrbuf;

    while (nleft > 0) {
        if ((nwritten = write(fd, bufp, nleft)) <= 0) {
            if (errno == EINTR) /* 시그널 핸들러가 끼어들었으면 */
                nwritten = 0;   /* 다시 write 한다 */
            else
                return -1;      /* write 가 errno 를 설정했다 */
        }
        nleft -= nwritten;
        bufp += nwritten;
    }
    return n;
}

/*
 * rio_read - 내부 버퍼에서 최대 n 바이트를 사용자 버퍼로 옮긴다.
 * 버퍼가 비어 있으면 read 로 다시 채운다. 0 이면 EOF, -1 이면 오류다.
 */
static ssize_t rio_read(rio_t *rp, char *usrbuf, size_t n)
{
    int cnt;

    while (rp->rio_cnt <= 0) { /* 버퍼가 비었으면 다시 채운다 */
        rp->rio_cnt = read(rp->rio_fd, rp->rio_buf, sizeof(rp->rio_buf));
        if (rp->rio_cnt < 0) {
            if (errno != EINTR) /* 시그널 핸들러 때문이 아니면 오류 */
                return -1;
        }
        else if (rp->rio_cnt == 0) /* EOF */
            return 0;
        else
            rp->rio_bufptr = rp->rio_buf; /* 읽을 위치를 처음으로 */
    }

    /* min(n, rp->rio_cnt) 바이트를 내부 버퍼에서 사용자 버퍼로 복사한다 */
    cnt = n;
    if ((size_t)rp->rio_cnt < n)
        cnt = rp->rio_cnt;
    memcpy(usrbuf, rp->rio_bufptr, cnt);
    rp->rio_bufptr += cnt;
    rp->rio_cnt -= cnt;
    return cnt;
}

/* rio_readinitb - 식별자 fd 와 읽기 버퍼 rp 를 연결한다 */
void rio_readinitb(rio_t *rp, int fd)
{
    rp->rio_fd = fd;
    rp->rio_cnt = 0;
    rp->rio_bufptr = rp->rio_buf;
}

/*
 * rio_readlineb - 줄바꿈('\n')까지 한 줄을 읽는다. 최대 maxlen-1 바이트를 읽고 NUL 로 끝낸다.
 * 리턴: 읽은 바이트 수, 아무것도 읽기 전에 EOF 면 0, 오류면 -1.
 */
ssize_t rio_readlineb(rio_t *rp, void *usrbuf, size_t maxlen)
{
    size_t n;
    int rc;
    char c, *bufp = usrbuf;

    for (n = 1; n < maxlen; n++) {
        if ((rc = rio_read(rp, &c, 1)) == 1) {
            *bufp++ = c;
            if (c == '\n') {
                n++;
                break;
            }
        } else if (rc == 0) {
            if (n == 1)
                return 0; /* EOF, 읽은 데이터 없음 */
            else
                break;    /* EOF, 일부 데이터를 읽음 */
        } else
            return -1;    /* 오류 */
    }
    *bufp = 0;
    return n - 1;
}

void Rio_writen(int fd, void *usrbuf, size_t n)
{
    if (rio_writen(fd, usrbuf, n) != (ssize_t)n)
        unix_error("Rio_writen error");
}

void Rio_readinitb(rio_t *rp, int fd)
{
    rio_readinitb(rp, fd);
}

ssize_t Rio_readlineb(rio_t *rp, void *usrbuf, size_t maxlen)
{
    ssize_t rc;

    if ((rc = rio_readlineb(rp, usrbuf, maxlen)) < 0)
        unix_error("Rio_readlineb error");
    return rc;
}

/********************************
 * 클라이언트·서버 도움 함수 (11.4.8)
 ********************************/

/*
 * open_clientfd - <hostname, port> 에서 듣고 있는 서버와 연결을 열고,
 * 읽기·쓰기가 가능한 소켓 식별자를 리턴한다. 그림 11.18.
 * 리턴: 성공하면 식별자, 모든 주소에 대해 연결이 실패하면 -1.
 */
int open_clientfd(char *hostname, char *port)
{
    int clientfd;
    struct addrinfo hints, *listp, *p;

    /* 연결할 수 있는 서버 주소 목록을 얻는다 */
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_socktype = SOCK_STREAM;  /* 연결을 연다 */
    hints.ai_flags = AI_NUMERICSERV;  /* ... port 는 숫자다 */
    hints.ai_flags |= AI_ADDRCONFIG;  /* 연결에 권장되는 플래그 */
    Getaddrinfo(hostname, port, &hints, &listp);

    /* 연결에 성공하는 주소를 찾을 때까지 목록을 따라간다 */
    for (p = listp; p; p = p->ai_next) {
        /* 소켓 식별자를 만든다 */
        if ((clientfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0)
            continue; /* socket 실패, 다음 주소로 */

        /* 서버에 연결한다 */
        if (connect(clientfd, p->ai_addr, p->ai_addrlen) != -1)
            break; /* 성공 */
        Close(clientfd); /* connect 실패, 소켓을 닫고 다음 주소로 */
    }

    /* 정리 */
    Freeaddrinfo(listp);
    if (!p) /* 모든 connect 가 실패 */
        return -1;
    else    /* 마지막 connect 가 성공 */
        return clientfd;
}

/*
 * open_listenfd - port 에서 연결 요청을 받을 준비가 된 듣기 식별자를 열고 리턴한다.
 * 그림 11.19.
 * 리턴: 성공하면 식별자, 실패하면 -1.
 */
int open_listenfd(char *port)
{
    struct addrinfo hints, *listp, *p;
    int listenfd, optval = 1;

    /* 들을 수 있는 서버 주소 목록을 얻는다 */
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_socktype = SOCK_STREAM;             /* 연결을 받는다 */
    hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG; /* ... 이 호스트의 모든 IP 주소로 */
    hints.ai_flags |= AI_NUMERICSERV;            /* ... port 는 숫자다 */
    Getaddrinfo(NULL, port, &hints, &listp);

    /* bind 에 성공하는 주소를 찾을 때까지 목록을 따라간다 */
    for (p = listp; p; p = p->ai_next) {
        /* 소켓 식별자를 만든다 */
        if ((listenfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0)
            continue; /* socket 실패, 다음 주소로 */

        /* bind 의 "Address already in use" 오류를 없앤다 */
        Setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
                   (const void *)&optval, sizeof(int));

        /* 식별자를 주소에 묶는다 */
        if (bind(listenfd, p->ai_addr, p->ai_addrlen) == 0)
            break; /* 성공 */
        Close(listenfd); /* bind 실패, 소켓을 닫고 다음 주소로 */
    }

    /* 정리 */
    Freeaddrinfo(listp);
    if (!p) /* 어떤 주소도 bind 하지 못함 */
        return -1;

    /* 연결 요청을 받을 수 있는 듣기 소켓으로 바꾼다 */
    if (listen(listenfd, LISTENQ) < 0) {
        Close(listenfd);
        return -1;
    }
    return listenfd;
}

int Open_clientfd(char *hostname, char *port)
{
    int rc;

    if ((rc = open_clientfd(hostname, port)) < 0)
        unix_error("Open_clientfd error");
    return rc;
}

int Open_listenfd(char *port)
{
    int rc;

    if ((rc = open_listenfd(port)) < 0)
        unix_error("Open_listenfd error");
    return rc;
}
