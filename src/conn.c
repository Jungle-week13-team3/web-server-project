#include "conn.h"

#include <stdatomic.h> /* atomic_int: 스레드 사이에 안전한 변수 (C11) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "cgi.h"
#include "http.h"
#include "net.h"
#include "proxy.h"
#include "static.h"

#define RESPONSE_HEAD_MAX 512

/* 종료 요청 표시. main 스레드의 시그널 처리 함수가 쓰고, 작업 스레드들이 읽는다.
 * atomic_int는 여러 스레드가 잠금 없이 동시에 읽고 써도 안전한 정수다.
 * (volatile sig_atomic_t는 시그널 처리 함수와 같은 스레드 사이에서만 안전해서, 스레드 사이에는 쓸 수 없다)
 * 잠금 없이 동작하는(lock-free) atomic이라 시그널 처리 함수 안에서 바꿔도 안전하다. */
static atomic_int shutting_down = 0;

/* 서버를 끄는 중이라고 표시한다. 시그널 처리 함수에서 불러도 안전하다(atomic 변수 하나만 바꾼다). */
void request_shutdown(void)
{
    atomic_store(&shutting_down, 1);
}

/* 서버를 끄는 중이면 1. 이때는 keep-alive를 하지 않고 지금 응답을 마지막으로 연결을 닫는다. */
int is_shutting_down(void)
{
    return atomic_load(&shutting_down);
}

/* 에러 상태 코드를 받아 "404 Not Found" 같은 문구를 본문으로 한 text/plain 응답을 보낸다. */
static void send_text_response(int connfd, int status, int keep_alive)
{
    char body[128];
    snprintf(body, sizeof(body), "%d %s\n", status, get_status_reason(status));
    size_t body_len = strlen(body);

    char head[RESPONSE_HEAD_MAX];
    int head_len = build_response_head(head, sizeof(head), status, "text/plain", body_len,
                                       keep_alive);
    if (head_len < 0)
        return;

    if (write_all(connfd, head, (size_t)head_len) < 0 ||
        write_all(connfd, body, body_len) < 0)
        perror("write");
}

/* 처리한 요청 한 줄과 결과 상태 코드를 서버 로그에 찍는다. */
static void log_request(const struct http_request *req, int status)
{
    if (req->method[0] == '\0')
        printf("request: (bad request line) -> %d\n", status);
    else
        printf("request: %s %s %s (body %zu) -> %d\n", req->method, req->uri, req->version,
               req->body_len, status);
    fflush(stdout);
}

/* 정상 요청을 URI 앞부분으로 갈래에 보내고 상태 코드를 돌려준다.
 * 프록시 규칙 → 뒤 서버, /cgi-bin/ → CGI, 그 외 → 정적 파일(GET만. 파일에 POST는 405).
 * *sent는 갈래가 응답을 이미 보냈으면 1이다. 0이면 부른 쪽이 에러 응답을 보낸다.
 * 갈래가 연결을 유지할 수 없게 되면 *keep_alive를 0으로 바꾼다. */
static int dispatch_request(int connfd, const struct server_config *config,
                            const struct http_request *req, int *keep_alive, int *sent)
{
    struct proxy_route *route = find_route(config->routes, req->uri);
    if (route != NULL)
        return serve_proxy(connfd, route, req, keep_alive, sent);

    int status;
    if (is_cgi_request(req->uri))
        status = serve_cgi(connfd, config->cgi_dir, req, *keep_alive);
    else if (strcmp(req->method, "GET") != 0)
        status = 405;
    else
        status = serve_static(connfd, config->static_root, req->uri, *keep_alive);
    *sent = (status == 200); /* 정적·CGI는 200일 때만 보낸다 */
    return status;
}

/* 연결에서 요청 하나를 읽고 응답 하나를 보낸다.
 * 요청 줄 → 빈 줄까지 헤더 → 상태 결정 → 본문 → 갈래별 응답 또는 에러 응답.
 * 같은 연결로 다음 요청을 받아도 되면 1, 닫아야 하면 0을 돌려준다. */
static int handle_one_request(int connfd, struct line_reader *reader,
                              const struct server_config *config)
{
    struct http_request req;
    memset(&req, 0, sizeof(req));
    char line[LINE_READER_BUF_SIZE];
    int status = 0; /* 0 = 아직 정하지 않음 */

    /* ① 요청 줄 */
    ssize_t n = read_line(reader, line, sizeof(line));
    if (n == READ_LINE_TOO_LONG) {
        status = 414; /* 요청 줄이 8KB를 넘는 건 거의 항상 URI가 길어서다 */
    } else if (n < 0) {
        /* 클라가 다 쓰고 닫았거나, 읽기 오류거나, 다음 요청을 기다리다 시간이 지났다. 답할 것 없이 닫는다 */
        return 0;
    } else {
        int rc = parse_request_line(line, &req);
        if (rc == PARSE_URI_TOO_LONG)
            status = 414;
        else if (rc < 0)
            status = 400;
    }

    /* ② 빈 줄이 나올 때까지 헤더 */
    while (status == 0) {
        n = read_line(reader, line, sizeof(line));
        if (n == READ_LINE_TOO_LONG) {
            status = 400;
            break;
        }
        if (n < 0) /* 헤더 도중에 끊기거나 시간이 지남 (불완전한 요청) */
            return 0;
        if (n == 0) /* 빈 줄 = 헤더 끝 */
            break;
        if (parse_header_line(line, &req) < 0)
            status = 400;
    }

    /* ③ 상태 결정, 본문 길이 확인 */
    if (status == 0)
        status = decide_request_status(&req);
    if (status == 200) {
        int rc = get_content_length(&req, &req.body_len);
        if (rc == CONTENT_LENGTH_INVALID)
            status = 400;
        else if (rc == CONTENT_LENGTH_TOO_LARGE)
            status = 413;
    }

    /* 여기까지 온 에러는 본문을 아직 안 읽은 상태다. 다음 요청이 어디서 시작하는지 모르니 닫는다 */
    if (status != 200) {
        log_request(&req, status);
        send_text_response(connfd, status, 0);
        return 0;
    }

    /* ④ 본문: Content-Length만큼 정확히 읽는다. 덜 읽으면 남은 본문을 다음 요청으로 착각한다 */
    if (req.body_len > 0) {
        req.body = malloc(req.body_len);
        if (req.body == NULL) {
            log_request(&req, 500);
            send_text_response(connfd, 500, 0);
            return 0;
        }
        if (read_buffered_exact(reader, req.body, req.body_len) != (ssize_t)req.body_len) {
            free(req.body); /* 본문 도중에 끊기거나 시간이 지남 */
            return 0;
        }
    }

    /* ⑤ 갈림길 → 응답. 서버가 꺼지는 중이면 이 응답을 마지막으로 연결을 닫는다 */
    int keep_alive = wants_keep_alive(&req) && !is_shutting_down();
    int sent = 0;
    status = dispatch_request(connfd, config, &req, &keep_alive, &sent);
    log_request(&req, status);
    if (!sent)
        send_text_response(connfd, status, keep_alive);

    free(req.body);
    return keep_alive;
}

/* 연결 하나에서 요청을 반복 처리한다. 클라가 닫거나, 닫자고 하거나, 시간이 지나면 돌아온다.
 * 소켓을 닫는 건 부른 쪽(main)이 한다. */
void serve_connection(int connfd, const struct server_config *config)
{
    /* 읽기·쓰기마다 최대 대기 시간을 건다. 가만히 있거나 응답을 안 받아 가는 클라가 작업 스레드를 붙잡지 못하게 한다 */
    struct timeval tv = { .tv_sec = KEEP_ALIVE_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(connfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* reader는 연결이 끝날 때까지 하나를 계속 쓴다. 앞 요청 뒤에 붙어 온 다음 요청 바이트가 여기 남아 있을 수 있다 */
    struct line_reader reader;
    init_line_reader(&reader, connfd);
    while (handle_one_request(connfd, &reader, config))
        ;
}
