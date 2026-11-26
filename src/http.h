#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

/* HTTP 요청을 해석하고 응답 머리를 만드는 함수들. 소켓은 모르고 문자열만 다룬다. */

#define HTTP_METHOD_MAX       16
#define HTTP_URI_MAX          4096
#define HTTP_VERSION_MAX      16
#define HTTP_HEADER_NAME_MAX  64
#define HTTP_HEADER_VALUE_MAX 2048
#define HTTP_MAX_HEADERS      32
#define HTTP_MAX_BODY         (1024 * 1024) /* 본문은 1MB까지. 넘으면 413 */

/* parse_request_line이 0(성공) 대신 돌려주는 값 */
#define PARSE_BAD_REQUEST  (-1) /* 형식이 틀림 → 400 */
#define PARSE_URI_TOO_LONG (-2) /* URI가 HTTP_URI_MAX를 넘음 → 414 */

/* get_content_length가 0(성공) 대신 돌려주는 값 */
#define CONTENT_LENGTH_INVALID   (-1) /* 숫자가 아니거나 여러 개 → 400 */
#define CONTENT_LENGTH_TOO_LARGE (-2) /* HTTP_MAX_BODY를 넘음 → 413 */

/* 헤더 한 줄. "Host: localhost:8888" → name="Host", value="localhost:8888" */
struct http_header {
    char name[HTTP_HEADER_NAME_MAX];
    char value[HTTP_HEADER_VALUE_MAX];
};

/* 요청 줄과 헤더를 해석한 결과 */
struct http_request {
    char method[HTTP_METHOD_MAX];   /* "GET" */
    char uri[HTTP_URI_MAX];         /* "/index.html" */
    char version[HTTP_VERSION_MAX]; /* "HTTP/1.1" */
    struct http_header headers[HTTP_MAX_HEADERS];
    int header_count;
    char *body;      /* POST 본문. 없으면 NULL. 읽은 쪽(conn.c)이 malloc하고 free한다 */
    size_t body_len; /* Content-Length 값 */
};

int parse_request_line(const char *line, struct http_request *req);
int parse_header_line(const char *line, struct http_request *req);
const char *find_header(const struct http_request *req, const char *name);
int decide_request_status(const struct http_request *req);
int get_content_length(const struct http_request *req, size_t *out);
int wants_keep_alive(const struct http_request *req);
const char *get_status_reason(int status);
int build_response_head(char *out, size_t cap, int status, const char *content_type,
                        size_t content_length, int keep_alive);

#endif
