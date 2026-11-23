/*
 * csapp.h - CS:APP 3판 csapp.h 중 11.4절(소켓 인터페이스, echo)에 필요한 부분만 옮긴 것.
 *
 * 함수 이름 규칙은 책과 같다.
 *   소문자 (open_clientfd, rio_readlineb ...) : 실패하면 -1 등을 리턴한다.
 *   대문자 (Open_clientfd, Rio_readlineb ...) : 실패하면 오류를 출력하고 프로세스를 끝낸다.
 */
#ifndef __CSAPP_H__
#define __CSAPP_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

/* connect, bind, accept 에 넘길 때 쓰는 포괄적 소켓 주소 타입 (11.4.1) */
typedef struct sockaddr SA;

/* RIO 버퍼 (10.5절). 소켓에서 한 줄씩 읽을 때 쓴다. */
#define RIO_BUFSIZE 8192
typedef struct {
    int rio_fd;                /* 이 버퍼가 읽는 식별자 */
    int rio_cnt;               /* 버퍼에 남은, 아직 읽지 않은 바이트 수 */
    char *rio_bufptr;          /* 버퍼에서 다음에 읽을 위치 */
    char rio_buf[RIO_BUFSIZE]; /* 내부 버퍼 */
} rio_t;

#define MAXLINE 8192 /* 텍스트 한 줄의 최대 길이 */
#define LISTENQ 1024 /* listen() 의 backlog 인자 */

/* 오류 출력 함수 */
void unix_error(char *msg);
void gai_error(int code, char *msg);
void app_error(char *msg);

/* Unix I/O 래퍼 */
void Close(int fd);

/* 소켓 인터페이스 래퍼 */
int Accept(int s, struct sockaddr *addr, socklen_t *addrlen);
void Setsockopt(int s, int level, int optname, const void *optval, int optlen);

/* 프로토콜에 무관한 주소 변환 래퍼 (11.4.7) */
void Getaddrinfo(const char *node, const char *service,
                 const struct addrinfo *hints, struct addrinfo **res);
void Getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host,
                 size_t hostlen, char *serv, size_t servlen, int flags);
void Freeaddrinfo(struct addrinfo *res);

/* 표준 I/O 래퍼 */
char *Fgets(char *ptr, int n, FILE *stream);
void Fputs(const char *ptr, FILE *stream);

/* RIO (Robust I/O) 패키지 */
ssize_t rio_writen(int fd, void *usrbuf, size_t n);
void rio_readinitb(rio_t *rp, int fd);
ssize_t rio_readlineb(rio_t *rp, void *usrbuf, size_t maxlen);

void Rio_writen(int fd, void *usrbuf, size_t n);
void Rio_readinitb(rio_t *rp, int fd);
ssize_t Rio_readlineb(rio_t *rp, void *usrbuf, size_t maxlen);

/* 클라이언트·서버 도움 함수 (11.4.8) */
int open_clientfd(char *hostname, char *port);
int open_listenfd(char *port);

int Open_clientfd(char *hostname, char *port);
int Open_listenfd(char *port);

#endif /* __CSAPP_H__ */
