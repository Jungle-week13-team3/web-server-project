/*
 * echo - 연결 식별자에서 텍스트 줄을 읽어 그대로 돌려보낸다. 그림 11.22.
 * 클라이언트가 연결을 닫아 EOF 를 만나면(Rio_readlineb 가 0) 리턴한다.
 */
#include "csapp.h"

void echo(int connfd)
{
    size_t n;
    char buf[MAXLINE];
    rio_t rio;

    Rio_readinitb(&rio, connfd);
    while ((n = Rio_readlineb(&rio, buf, MAXLINE)) != 0) {
        printf("server received %d bytes\n", (int)n);
        Rio_writen(connfd, buf, n);
    }
}
