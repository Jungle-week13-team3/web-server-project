#ifndef NET_H
#define NET_H

#include <stddef.h>
#include <sys/types.h>

/* 소켓을 열고, 끝까지 읽고 쓰는 공용 부품. 무슨 내용을 주고받는지는 모른다. */

int open_listenfd(const char *port);
int open_clientfd(const char *host, const char *port);
ssize_t read_all(int fd, void *buf, size_t len);
int write_all(int fd, const void *buf, size_t len);

#endif
