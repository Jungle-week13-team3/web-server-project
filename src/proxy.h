#ifndef PROXY_H
#define PROXY_H

#include <stddef.h>

#include "http.h"

/* Reverse Proxy: 경로 앞부분이 맞는 요청을 뒤 서버(백엔드)에 넘기고, 받은 응답을 클라에 그대로 전달한다. */

#define PROXY_MAX_ROUTES   8
#define PROXY_MAX_BACKENDS 8
#define PROXY_PREFIX_MAX   64
#define PROXY_HOST_MAX     64
#define PROXY_PORT_MAX     8
#define BACKEND_TIMEOUT_SEC 5    /* 뒤 서버 읽기·쓰기 한 번의 최대 대기. 넘으면 504 */
#define PROXY_TIMED_OUT    (-2)

/* 뒤 서버 하나의 주소. "127.0.0.1:9001" → host "127.0.0.1", port "9001" */
struct proxy_backend {
    char host[PROXY_HOST_MAX];
    char port[PROXY_PORT_MAX];
};

/* 라우팅 규칙 한 줄. "/api/ /cgi-bin/ 127.0.0.1:9001 127.0.0.1:9002"
 * → "/api/"로 시작하는 요청을 앞부분만 "/cgi-bin/"으로 바꿔 두 서버에 번갈아 보낸다 */
struct proxy_route {
    char prefix[PROXY_PREFIX_MAX];  /* 이 앞부분으로 시작하는 URI만 */
    char rewrite[PROXY_PREFIX_MAX]; /* 뒤 서버에 보낼 때 prefix 자리에 넣을 것 */
    struct proxy_backend backends[PROXY_MAX_BACKENDS];
    int backend_count;
    int next; /* Round-Robin: 다음에 고를 뒤 서버 번호 */
};

#define FORWARD_MAX_HOSTS 8

/* 설정 파일에서 읽은 규칙 전체 */
struct proxy_table {
    struct proxy_route routes[PROXY_MAX_ROUTES];
    int count;
    /* 포워드 프록시: 설정 파일에 "forward 127.0.0.1 localhost" 줄이 있으면 켜진다.
     * 아무 곳에나 중계하면 남이 우리 서버를 거쳐 다른 곳을 공격할 수 있어서, 적힌 호스트로만 보낸다 */
    int forward_enabled;
    char forward_hosts[FORWARD_MAX_HOSTS][PROXY_HOST_MAX];
    int forward_host_count;
};

/* parse_absolute_uri가 0(성공) 대신 돌려주는 값 */
#define ABSOLUTE_URI_BAD   (-1) /* 형식이 틀림 → 400 */
#define ABSOLUTE_URI_HTTPS (-2) /* https는 지원하지 않음 → 501 */

/* parse_route_line이 돌려주는 값 */
#define ROUTE_LINE_OK    0
#define ROUTE_LINE_SKIP  1    /* 빈 줄이나 '#' 주석 */
#define ROUTE_LINE_ERROR (-1)

int parse_route_line(const char *line, struct proxy_route *route);
int load_route_file(const char *path, struct proxy_table *table);
struct proxy_route *find_route(struct proxy_table *table, const char *uri);
int rewrite_uri(const struct proxy_route *route, const char *uri, char *out, size_t cap);
int pick_backend(struct proxy_route *route);
int build_backend_request(const struct http_request *req, const char *uri, char *out,
                          size_t cap);
int serve_proxy(int connfd, struct proxy_route *route, const struct http_request *req,
                int *keep_alive, int *sent);

int parse_forward_line(const char *line, struct proxy_table *table);
int is_absolute_uri(const char *uri);
int parse_absolute_uri(const char *uri, char *host, size_t host_cap, char *port, size_t port_cap,
                       char *path, size_t path_cap);
int is_forward_allowed(const struct proxy_table *table, const char *host);
int serve_forward_proxy(int connfd, const struct proxy_table *table, const struct http_request *req,
                        int *keep_alive, int *sent);

#endif
