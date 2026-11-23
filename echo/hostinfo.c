/*
 * hostinfo - 도메인 이름에 연결된 IPv4 주소를 모두 출력한다. 그림 11.17.
 * 사용법: ./hostinfo <domain name>
 */
#include "csapp.h"

int main(int argc, char **argv)
{
    struct addrinfo *p, *listp, hints;
    char buf[MAXLINE];
    int rc, flags;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <domain name>\n", argv[0]);
        exit(0);
    }

    /* addrinfo 레코드 목록을 얻는다 */
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_INET;       /* IPv4 만 */
    hints.ai_socktype = SOCK_STREAM; /* 연결용 주소만 */
    if ((rc = getaddrinfo(argv[1], NULL, &hints, &listp)) != 0) {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(rc));
        exit(1);
    }

    /* 목록을 따라가며 IP 주소를 출력한다 */
    flags = NI_NUMERICHOST; /* 도메인 이름 대신 주소 문자열을 출력한다 */
    for (p = listp; p; p = p->ai_next) {
        Getnameinfo(p->ai_addr, p->ai_addrlen, buf, MAXLINE, NULL, 0, flags);
        printf("%s\n", buf);
    }

    /* 정리 */
    Freeaddrinfo(listp);

    exit(0);
}
