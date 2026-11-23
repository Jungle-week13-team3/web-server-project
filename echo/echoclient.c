/*
 * echoclient - 표준 입력의 줄을 서버로 보내고, 돌아온 줄을 표준 출력에 쓴다. 그림 11.20.
 * 사용법: ./echoclient <host> <port>
 * 표준 입력이 EOF(Ctrl+D)가 되면 연결을 닫고 끝난다.
 */
#include "csapp.h"

int main(int argc, char **argv)
{
    int clientfd;
    char *host, *port, buf[MAXLINE];
    rio_t rio;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <host> <port>\n", argv[0]);
        exit(0);
    }
    host = argv[1];
    port = argv[2];

    clientfd = Open_clientfd(host, port);
    Rio_readinitb(&rio, clientfd);

    while (Fgets(buf, MAXLINE, stdin) != NULL) {
        Rio_writen(clientfd, buf, strlen(buf));
        Rio_readlineb(&rio, buf, MAXLINE);
        Fputs(buf, stdout);
    }
    Close(clientfd);
    exit(0);
}
