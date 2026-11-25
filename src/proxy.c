#include "proxy.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp, strncasecmp */
#include <unistd.h>

#include "net.h"

#define PROXY_HEAD_MAX (80 * 1024) /* 요청·응답 머리를 담는 버퍼. 헤더 32개 × 2KB를 넘게 잡는다 */
#define PROXY_COPY_BUF 8192

/* 홉(hop) 하나에만 해당하는 헤더인지 본다. 연결 방식은 클라↔프록시, 프록시↔뒤 서버가 따로 정하므로 넘기지 않는다.
 * Expect는 프록시가 본문을 이미 다 받아 둔 뒤라서 뒤 서버에 넘길 이유가 없다. */
static int is_hop_by_hop(const char *name)
{
    static const char *names[] = { "Connection", "Keep-Alive", "Proxy-Connection",
                                   "TE", "Upgrade", "Expect" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcasecmp(name, names[i]) == 0)
            return 1;
    }
    return 0;
}

/* "127.0.0.1:9001"을 host와 port로 나눠 backend에 넣는다. 성공 0, 형식이 틀리면 -1. */
static int parse_backend(const char *text, size_t len, struct proxy_backend *backend)
{
    const char *colon = memchr(text, ':', len);
    if (colon == NULL)
        return -1;
    size_t host_len = (size_t)(colon - text);
    size_t port_len = len - host_len - 1;
    if (host_len == 0 || host_len >= sizeof(backend->host) ||
        port_len == 0 || port_len >= sizeof(backend->port))
        return -1;
    for (size_t i = 0; i < port_len; i++) {
        if (colon[1 + i] < '0' || colon[1 + i] > '9')
            return -1;
    }
    memcpy(backend->host, text, host_len);
    backend->host[host_len] = '\0';
    memcpy(backend->port, colon + 1, port_len);
    backend->port[port_len] = '\0';
    return 0;
}

/* 설정 파일 한 줄 "/api/ /cgi-bin/ 127.0.0.1:9001 127.0.0.1:9002"를 route에 채운다.
 * 칸은 공백으로 나뉜다: 앞부분, 바꿀 앞부분, 뒤 서버 1개 이상.
 * 성공 ROUTE_LINE_OK, 빈 줄·주석이면 ROUTE_LINE_SKIP, 틀리면 ROUTE_LINE_ERROR. */
int parse_route_line(const char *line, struct proxy_route *route)
{
    memset(route, 0, sizeof(*route));
    const char *p = line;
    int field = 0;

    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (*p == '\0')
            break;
        if (*p == '#') {
            if (field == 0)
                return ROUTE_LINE_SKIP;
            break; /* 줄 끝 주석 */
        }
        const char *start = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
            p++;
        size_t len = (size_t)(p - start);

        if (field == 0 || field == 1) {
            char *dst = (field == 0) ? route->prefix : route->rewrite;
            if (start[0] != '/' || len >= PROXY_PREFIX_MAX)
                return ROUTE_LINE_ERROR;
            memcpy(dst, start, len);
            dst[len] = '\0';
        } else {
            if (route->backend_count >= PROXY_MAX_BACKENDS ||
                parse_backend(start, len, &route->backends[route->backend_count]) < 0)
                return ROUTE_LINE_ERROR;
            route->backend_count++;
        }
        field++;
    }

    if (field == 0)
        return ROUTE_LINE_SKIP;
    if (route->backend_count == 0) /* 앞부분, 바꿀 앞부분, 뒤 서버까지 다 있어야 한다 */
        return ROUTE_LINE_ERROR;
    return ROUTE_LINE_OK;
}

/* 설정 파일을 한 줄씩 읽어 table에 규칙을 채운다. 성공 0, 파일을 못 열거나 틀린 줄이 있으면 -1(줄 번호를 출력). */
int load_route_file(const char *path, struct proxy_table *table)
{
    memset(table, 0, sizeof(*table));
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        perror(path);
        return -1;
    }
    char line[1024];
    int line_no = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        line_no++;
        struct proxy_route route;
        int rc = parse_route_line(line, &route);
        if (rc == ROUTE_LINE_SKIP)
            continue;
        if (rc == ROUTE_LINE_ERROR || table->count >= PROXY_MAX_ROUTES) {
            fprintf(stderr, "%s:%d: bad route line\n", path, line_no);
            fclose(fp);
            return -1;
        }
        table->routes[table->count++] = route;
    }
    fclose(fp);
    return 0;
}

/* URI가 앞부분으로 시작하는 첫 규칙을 돌려준다. 맞는 규칙이 없으면 NULL. */
struct proxy_route *find_route(struct proxy_table *table, const char *uri)
{
    if (table == NULL)
        return NULL;
    for (int i = 0; i < table->count; i++) {
        const char *prefix = table->routes[i].prefix;
        if (strncmp(uri, prefix, strlen(prefix)) == 0)
            return &table->routes[i];
    }
    return NULL;
}

/* URI의 앞부분을 바꿔 out에 쓴다. "/api/adder?a=1" → "/cgi-bin/adder?a=1". 성공 0, out이 모자라면 -1. */
int rewrite_uri(const struct proxy_route *route, const char *uri, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s%s", route->rewrite, uri + strlen(route->prefix));
    if (n < 0 || (size_t)n >= cap)
        return -1;
    return 0;
}

/* Round-Robin 순번을 지키는 잠금. 모든 규칙이 같이 쓴다(순번 바꾸기는 아주 짧아서 하나로 충분하다) */
static pthread_mutex_t rr_lock = PTHREAD_MUTEX_INITIALIZER;

/* Round-Robin: 뒤 서버를 순서대로 하나씩 돌아가며 고른다. 고른 번호를 돌려준다.
 * 작업 스레드 여럿이 동시에 부르므로 잠그고 바꾼다. 안 잠그면 두 스레드가 같은 번호를 받을 수 있다. */
int pick_backend(struct proxy_route *route)
{
    pthread_mutex_lock(&rr_lock);
    int index = route->next;
    route->next = (route->next + 1) % route->backend_count;
    pthread_mutex_unlock(&rr_lock);
    return index;
}

/* 뒤 서버에 보낼 요청 머리를 out에 만든다. 요청 줄은 바꾼 uri로, 헤더는 홉 헤더만 빼고 그대로,
 * 마지막에 "Connection: close"를 붙인다(뒤 서버와는 요청 하나마다 연결을 새로 연다). 만든 길이, 모자라면 -1. */
int build_backend_request(const struct http_request *req, const char *uri, char *out, size_t cap)
{
    size_t used = 0;
    int n = snprintf(out, cap, "%s %s HTTP/1.1\r\n", req->method, uri);
    if (n < 0 || (size_t)n >= cap)
        return -1;
    used = (size_t)n;

    for (int i = 0; i < req->header_count; i++) {
        if (is_hop_by_hop(req->headers[i].name))
            continue;
        n = snprintf(out + used, cap - used, "%s: %s\r\n", req->headers[i].name,
                     req->headers[i].value);
        if (n < 0 || (size_t)n >= cap - used)
            return -1;
        used += (size_t)n;
    }

    n = snprintf(out + used, cap - used, "Connection: close\r\n\r\n");
    if (n < 0 || (size_t)n >= cap - used)
        return -1;
    return (int)(used + (size_t)n);
}

/* out(크기 cap, 지금 used만큼 참)의 끝에 line과 "\r\n"을 붙인다. 자리가 없으면 -1. */
static int append_line(char *out, size_t cap, size_t *used, const char *line)
{
    int n = snprintf(out + *used, cap - *used, "%s\r\n", line);
    if (n < 0 || (size_t)n >= cap - *used)
        return -1;
    *used += (size_t)n;
    return 0;
}

/* 뒤 서버 응답을 읽어 클라에 넘긴다. 상태 줄과 헤더는 Connection만 클라 쪽 값으로 바꾸고, 본문은 바이트 그대로 보낸다.
 * 클라에 아무것도 보내기 전에 실패하면 -1(→ 502). 보내기 시작했으면 *sent = 1, 뒤 서버의 상태 코드를 돌려준다. */
static int relay_response(int backend_fd, int connfd, char *head, int *keep_alive, int *sent)
{
    struct line_reader reader;
    char line[LINE_READER_BUF_SIZE];
    init_line_reader(&reader, backend_fd);

    /* 상태 줄 "HTTP/1.1 200 OK"에서 상태 코드만 꺼내 둔다 (로그용) */
    if (read_line(&reader, line, sizeof(line)) <= 0)
        return -1;
    int status;
    if (sscanf(line, "HTTP/%*d.%*d %d", &status) != 1 || status < 100 || status > 999)
        return -1;
    size_t used = 0;
    if (append_line(head, PROXY_HEAD_MAX, &used, line) < 0)
        return -1;

    /* 헤더: Content-Length로 본문 끝을 알아 둔다. 없으면 뒤 서버가 닫을 때까지가 본문이다 */
    int has_length = 0;
    size_t body_len = 0;
    for (;;) {
        ssize_t n = read_line(&reader, line, sizeof(line));
        if (n < 0)
            return -1;
        if (n == 0)
            break;
        char *colon = strchr(line, ':');
        if (colon == NULL)
            return -1;
        *colon = '\0';
        const char *value = colon + 1;
        while (*value == ' ' || *value == '\t')
            value++;
        if (strcasecmp(line, "Connection") == 0 || strcasecmp(line, "Keep-Alive") == 0)
            continue;
        if (strcasecmp(line, "Content-Length") == 0) {
            char *end;
            errno = 0;
            unsigned long long v = strtoull(value, &end, 10);
            if (end == value || *end != '\0' || errno != 0 || value[0] == '-')
                return -1;
            has_length = 1;
            body_len = (size_t)v;
        }
        *colon = ':';
        if (append_line(head, PROXY_HEAD_MAX, &used, line) < 0)
            return -1;
    }

    /* 길이를 모르면 본문 끝 = 연결 끝이라서, 클라와의 연결도 닫아야 클라가 끝을 안다 */
    if (!has_length)
        *keep_alive = 0;
    int n = snprintf(head + used, PROXY_HEAD_MAX - used, "Connection: %s\r\n\r\n",
                     *keep_alive ? "keep-alive" : "close");
    if (n < 0 || (size_t)n >= PROXY_HEAD_MAX - used)
        return -1;
    used += (size_t)n;

    *sent = 1;
    if (write_all(connfd, head, used) < 0) {
        *keep_alive = 0;
        return status;
    }

    /* 본문: 이미지도 있으니 줄이 아니라 바이트로 옮긴다. 헤더를 읽다 reader에 남은 본문부터 나온다 */
    char buf[PROXY_COPY_BUF];
    size_t left = body_len;
    while (!has_length || left > 0) {
        size_t want = sizeof(buf);
        if (has_length && left < want)
            want = left;
        ssize_t got = read_some(&reader, buf, want);
        if (got == 0 && !has_length) /* 길이 없는 본문의 정상 끝 */
            break;
        if (got <= 0 || write_all(connfd, buf, (size_t)got) < 0) {
            *keep_alive = 0; /* 본문이 덜 갔다. 클라 쪽 연결 상태를 믿을 수 없으니 닫는다 */
            break;
        }
        left -= (size_t)got;
    }
    return status;
}

/* 요청을 규칙에 맞는 뒤 서버로 넘기고 응답을 클라에 전달한다.
 * 뒤 서버를 Round-Robin으로 고르고, 접속이 안 되면 다음 서버를 시도한다.
 * *sent가 1이면 응답을 이미 보냈다(상태 코드는 로그용). 0이면 부른 쪽이 돌려준 에러 코드로 답한다. */
int serve_proxy(int connfd, struct proxy_route *route, const struct http_request *req,
                int *keep_alive, int *sent)
{
    *sent = 0;
    char uri[HTTP_URI_MAX];
    if (rewrite_uri(route, req->uri, uri, sizeof(uri)) < 0)
        return 414;

    char *head = malloc(PROXY_HEAD_MAX);
    if (head == NULL)
        return 500;
    int head_len = build_backend_request(req, uri, head, PROXY_HEAD_MAX);
    if (head_len < 0) {
        free(head);
        return 500;
    }

    /* 차례인 서버부터 시도하고, 죽어 있으면 다음 서버로 넘어간다 */
    int backend_fd = -1;
    for (int tries = 0; tries < route->backend_count && backend_fd < 0; tries++) {
        const struct proxy_backend *b = &route->backends[pick_backend(route)];
        backend_fd = connect_to_server(b->host, b->port);
        if (backend_fd < 0)
            fprintf(stderr, "proxy: backend %s:%s unavailable\n", b->host, b->port);
    }
    if (backend_fd < 0) {
        free(head);
        return 502; /* 뒤 서버가 전부 응답하지 않는다 */
    }

    int status = 502;
    if (write_all(backend_fd, head, (size_t)head_len) == 0 &&
        (req->body_len == 0 || write_all(backend_fd, req->body, req->body_len) == 0)) {
        int rc = relay_response(backend_fd, connfd, head, keep_alive, sent);
        if (rc > 0)
            status = rc;
    }
    close(backend_fd);
    free(head);
    return status;
}
