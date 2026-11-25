#ifndef NET_H
#define NET_H

#include <stddef.h>
#include <sys/types.h>

/* 소켓을 열고, 끝까지 읽고 쓰는 공용 부품. 무슨 내용을 주고받는지는 모른다. */

int open_listen_socket(const char *port);
int connect_to_server(const char *host, const char *port);
ssize_t read_exact(int fd, void *buf, size_t len);
int write_all(int fd, const void *buf, size_t len);
void drain_and_close(int fd);

/* 소켓에서 한 줄씩 꺼내 읽기 위한 버퍼.
 * read를 한 바이트씩 부르면 느려서, 한 번에 많이 받아 두고 여기서 줄을 잘라 준다. */
#define LINE_READER_BUF_SIZE 8192

struct line_reader {
    int fd;                          /* 읽을 소켓 */
    char buf[LINE_READER_BUF_SIZE];  /* read로 받아 둔 바이트 */
    size_t start;                    /* buf에서 아직 안 꺼낸 첫 위치 */
    size_t end;                      /* buf에 채워진 끝 위치 */
};

/* read_line이 0 이상(줄 길이) 대신 돌려주는 값 */
#define READ_LINE_EOF      (-1) /* 줄이 끝나기 전에 상대가 연결을 닫았다 */
#define READ_LINE_ERROR    (-2) /* read 오류 */
#define READ_LINE_TOO_LONG (-3) /* 줄이 out 버퍼보다 길다 */

void init_line_reader(struct line_reader *reader, int fd);
ssize_t read_line(struct line_reader *reader, char *out, size_t cap);
ssize_t read_buffered_exact(struct line_reader *reader, void *buf, size_t len);
ssize_t read_some(struct line_reader *reader, void *buf, size_t cap);

#endif
