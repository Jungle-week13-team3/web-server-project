/* http.c 함수들을 소켓 없이 문자열로 직접 불러 확인하는 단위 테스트.
 * assert가 하나라도 틀리면 그 줄 번호를 출력하고 멈춘다. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "http.h"

/* 정상 요청 줄과 칸 수가 틀린 요청 줄을 확인한다. */
static void test_parse_request_line(void)
{
    struct http_request req;

    assert(parse_request_line("GET /index.html HTTP/1.1", &req) == 0);
    assert(strcmp(req.method, "GET") == 0);
    assert(strcmp(req.uri, "/index.html") == 0);
    assert(strcmp(req.version, "HTTP/1.1") == 0);
    assert(req.header_count == 0);

    assert(parse_request_line("POST /api?a=1 HTTP/1.0", &req) == 0);
    assert(strcmp(req.uri, "/api?a=1") == 0);

    assert(parse_request_line("", &req) == -1);                         /* 빈 줄 */
    assert(parse_request_line("GET /index.html", &req) == -1);          /* 칸 2개 */
    assert(parse_request_line("GET / HTTP/1.1 extra", &req) == -1);     /* 칸 4개 */
    assert(parse_request_line("GET  / HTTP/1.1", &req) == -1);          /* 공백 2개 → 빈 칸 */
    assert(parse_request_line("GET index.html HTTP/1.1", &req) == -1);  /* '/'로 시작 안 함 */
    assert(parse_request_line("GET ftp://x/ HTTP/1.1", &req) == -1);    /* http 말고 다른 주소 */

    /* 포워드 프록시용 전체 주소와 CONNECT는 형식으로는 통과한다 */
    assert(parse_request_line("GET http://127.0.0.1:9001/a?b=1 HTTP/1.1", &req) == 0);
    assert(strcmp(req.uri, "http://127.0.0.1:9001/a?b=1") == 0);
    assert(parse_request_line("GET HTTPS://x/ HTTP/1.1", &req) == 0);
    assert(parse_request_line("CONNECT example.com:443 HTTP/1.1", &req) == 0);
    assert(parse_request_line("GET / FTP/1.1", &req) == -1);            /* HTTP/로 시작 안 함 */
    assert(parse_request_line("GET / ", &req) == -1);                   /* 버전 칸이 빔 */

    /* 메서드 칸이 버퍼(16)보다 길면 실패 */
    assert(parse_request_line("AAAAAAAAAAAAAAAAAAAA / HTTP/1.1", &req) == -1);

    /* URI만 너무 길면 400이 아니라 414로 답할 수 있게 따로 알려 준다 */
    static char long_line[HTTP_URI_MAX + 32];
    strcpy(long_line, "GET /");
    memset(long_line + 5, 'a', HTTP_URI_MAX);
    strcpy(long_line + 5 + HTTP_URI_MAX, " HTTP/1.1");
    assert(parse_request_line(long_line, &req) == PARSE_URI_TOO_LONG);
}

/* 헤더 줄 나누기, 공백 제거, 잘못된 헤더를 확인한다. */
static void test_parse_header_line(void)
{
    struct http_request req;
    assert(parse_request_line("GET / HTTP/1.1", &req) == 0);

    assert(parse_header_line("Host: localhost:8888", &req) == 0);
    assert(req.header_count == 1);
    assert(strcmp(req.headers[0].name, "Host") == 0);
    assert(strcmp(req.headers[0].value, "localhost:8888") == 0); /* 값 안의 ':'는 그대로 */

    assert(parse_header_line("Connection:   close  ", &req) == 0);
    assert(strcmp(req.headers[1].value, "close") == 0);          /* 앞뒤 공백 제거 */

    assert(parse_header_line("X-Empty:", &req) == 0);            /* 빈 값은 허용 */
    assert(strcmp(req.headers[2].value, "") == 0);

    assert(parse_header_line("Host localhost", &req) == -1);     /* ':' 없음 */
    assert(parse_header_line(": value", &req) == -1);            /* 이름이 빔 */
    assert(parse_header_line("Host : x", &req) == -1);           /* 이름 뒤 공백 */
    assert(req.header_count == 3);                               /* 실패한 줄은 안 붙는다 */
}

/* 헤더 개수 상한을 넘으면 실패하는지 확인한다. */
static void test_too_many_headers(void)
{
    struct http_request req;
    assert(parse_request_line("GET / HTTP/1.1", &req) == 0);
    for (int i = 0; i < HTTP_MAX_HEADERS; i++)
        assert(parse_header_line("X-A: 1", &req) == 0);
    assert(parse_header_line("X-A: 1", &req) == -1);
}

/* 헤더 이름을 대소문자 구분 없이 찾는지 확인한다. */
static void test_find_header(void)
{
    struct http_request req;
    assert(parse_request_line("GET / HTTP/1.1", &req) == 0);
    assert(parse_header_line("Content-Length: 12", &req) == 0);

    assert(strcmp(find_header(&req, "Content-Length"), "12") == 0);
    assert(strcmp(find_header(&req, "content-length"), "12") == 0);
    assert(find_header(&req, "Host") == NULL);
}

/* 메서드·버전·Host 조합에 따라 상태 코드가 맞게 정해지는지 확인한다. */
static void test_decide_request_status(void)
{
    struct http_request req;

    parse_request_line("GET / HTTP/1.1", &req);
    parse_header_line("Host: x", &req);
    assert(decide_request_status(&req) == 200);

    parse_request_line("POST / HTTP/1.1", &req);
    parse_header_line("host: x", &req);                /* 소문자 host도 인정 */
    assert(decide_request_status(&req) == 200);

    parse_request_line("GET / HTTP/1.0", &req);        /* 1.0은 Host 없어도 된다 */
    assert(decide_request_status(&req) == 200);

    parse_request_line("GET / HTTP/1.1", &req);        /* 1.1인데 Host 없음 */
    assert(decide_request_status(&req) == 400);

    parse_request_line("GET / HTTP/1.1", &req);        /* Host 두 개 */
    parse_header_line("Host: a", &req);
    parse_header_line("Host: b", &req);
    assert(decide_request_status(&req) == 400);

    parse_request_line("DELETE / HTTP/1.1", &req);
    parse_header_line("Host: x", &req);
    assert(decide_request_status(&req) == 501);

    parse_request_line("get / HTTP/1.1", &req);        /* 메서드는 대소문자를 가린다 */
    parse_header_line("Host: x", &req);
    assert(decide_request_status(&req) == 501);

    parse_request_line("GET / HTTP/2.0", &req);
    parse_header_line("Host: x", &req);
    assert(decide_request_status(&req) == 505);

    parse_request_line("POST / HTTP/1.1", &req);       /* chunked 본문은 지원 안 함 */
    parse_header_line("Host: x", &req);
    parse_header_line("Transfer-Encoding: chunked", &req);
    assert(decide_request_status(&req) == 501);
}

/* Content-Length 값을 숫자로 바꾸고, 틀린 값과 너무 큰 값을 구분하는지 확인한다. */
static void test_get_content_length(void)
{
    struct http_request req;
    size_t len;

    parse_request_line("GET / HTTP/1.1", &req);        /* 헤더 없음 = 본문 0 */
    assert(get_content_length(&req, &len) == 0 && len == 0);

    parse_request_line("POST / HTTP/1.1", &req);
    parse_header_line("Content-Length: 7", &req);
    assert(get_content_length(&req, &len) == 0 && len == 7);

    parse_request_line("POST / HTTP/1.1", &req);
    parse_header_line("Content-Length: 1048576", &req); /* 딱 1MB는 허용 */
    assert(get_content_length(&req, &len) == 0 && len == HTTP_MAX_BODY);

    const char *invalid[] = { "Content-Length: abc", "Content-Length: -1", "Content-Length: +5",
                              "Content-Length: 5 5", "Content-Length:" };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        parse_request_line("POST / HTTP/1.1", &req);
        parse_header_line(invalid[i], &req);
        assert(get_content_length(&req, &len) == CONTENT_LENGTH_INVALID);
    }

    parse_request_line("POST / HTTP/1.1", &req);       /* 두 번 나옴 */
    parse_header_line("Content-Length: 3", &req);
    parse_header_line("Content-Length: 3", &req);
    assert(get_content_length(&req, &len) == CONTENT_LENGTH_INVALID);

    parse_request_line("POST / HTTP/1.1", &req);
    parse_header_line("Content-Length: 1048577", &req);
    assert(get_content_length(&req, &len) == CONTENT_LENGTH_TOO_LARGE);

    parse_request_line("POST / HTTP/1.1", &req);       /* 아주 큰 숫자도 넘치지 않고 413 */
    parse_header_line("Content-Length: 99999999999999999999999999", &req);
    assert(get_content_length(&req, &len) == CONTENT_LENGTH_TOO_LARGE);
}

/* 버전과 Connection 헤더 조합으로 연결 유지 여부가 맞게 정해지는지 확인한다. */
static void test_wants_keep_alive(void)
{
    struct http_request req;

    parse_request_line("GET / HTTP/1.1", &req);        /* 1.1 기본은 유지 */
    assert(wants_keep_alive(&req) == 1);
    parse_header_line("Connection: Close", &req);      /* 값도 대소문자 무시 */
    assert(wants_keep_alive(&req) == 0);

    parse_request_line("GET / HTTP/1.0", &req);        /* 1.0 기본은 닫기 */
    assert(wants_keep_alive(&req) == 0);
    parse_header_line("Connection: keep-alive", &req);
    assert(wants_keep_alive(&req) == 1);
}

/* 응답 머리가 정확한 문자열로 만들어지는지, 버퍼가 모자라면 실패하는지 확인한다. */
static void test_build_response_head(void)
{
    char out[256];
    int n = build_response_head(out, sizeof(out), 200, "text/plain", 6, 0);
    const char *expected =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 6\r\n"
        "Connection: close\r\n"
        "\r\n";
    assert(n == (int)strlen(expected));
    assert(strcmp(out, expected) == 0);

    n = build_response_head(out, sizeof(out), 200, "text/plain", 6, 1);
    assert(n > 0 && strstr(out, "Connection: keep-alive\r\n") != NULL);

    n = build_response_head(out, sizeof(out), 404, "text/html", 0, 0);
    assert(n > 0);
    assert(strncmp(out, "HTTP/1.1 404 Not Found\r\n", 24) == 0);

    char small[10];
    assert(build_response_head(small, sizeof(small), 200, "text/plain", 6, 0) == -1);
}

int main(void)
{
    test_parse_request_line();
    test_parse_header_line();
    test_too_many_headers();
    test_find_header();
    test_decide_request_status();
    test_get_content_length();
    test_wants_keep_alive();
    test_build_response_head();
    printf("all http unit tests passed\n");
    return 0;
}
