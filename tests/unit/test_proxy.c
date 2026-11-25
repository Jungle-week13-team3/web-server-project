/* proxy.c의 순수 함수(규칙 해석, 규칙 찾기, URI 바꾸기, Round-Robin, 요청 머리 만들기)를 소켓 없이 확인하는 단위 테스트. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "proxy.h"

/* 규칙 한 줄을 칸별로 나누고, 빈 줄·주석·틀린 줄을 구분하는지 확인한다. */
static void test_parse_route_line(void)
{
    struct proxy_route r;

    assert(parse_route_line("/api/ /cgi-bin/ 127.0.0.1:9001 127.0.0.1:9002\n", &r) == ROUTE_LINE_OK);
    assert(strcmp(r.prefix, "/api/") == 0);
    assert(strcmp(r.rewrite, "/cgi-bin/") == 0);
    assert(r.backend_count == 2);
    assert(strcmp(r.backends[0].host, "127.0.0.1") == 0);
    assert(strcmp(r.backends[1].port, "9002") == 0);
    assert(r.next == 0);

    assert(parse_route_line("  /img/\t/\tlocalhost:80  # 끝 주석\n", &r) == ROUTE_LINE_OK);
    assert(strcmp(r.backends[0].host, "localhost") == 0 && r.backend_count == 1);

    assert(parse_route_line("\n", &r) == ROUTE_LINE_SKIP);                     /* 빈 줄 */
    assert(parse_route_line("   # 주석\n", &r) == ROUTE_LINE_SKIP);
    assert(parse_route_line("/api/ /cgi-bin/\n", &r) == ROUTE_LINE_ERROR);     /* 뒤 서버 없음 */
    assert(parse_route_line("api/ /x/ h:1\n", &r) == ROUTE_LINE_ERROR);        /* '/'로 시작 안 함 */
    assert(parse_route_line("/a/ /b/ 127.0.0.1\n", &r) == ROUTE_LINE_ERROR);   /* 포트 없음 */
    assert(parse_route_line("/a/ /b/ host:9x\n", &r) == ROUTE_LINE_ERROR);     /* 포트가 숫자 아님 */
    assert(parse_route_line("/a/ /b/ :9001\n", &r) == ROUTE_LINE_ERROR);       /* 호스트 없음 */
}

/* 앞부분으로 규칙을 찾고, 앞부분만 바꾸는지 확인한다. */
static void test_find_and_rewrite(void)
{
    struct proxy_table t;
    memset(&t, 0, sizeof(t));
    assert(parse_route_line("/api/ /cgi-bin/ h:1", &t.routes[0]) == ROUTE_LINE_OK);
    assert(parse_route_line("/img/ / h:2", &t.routes[1]) == ROUTE_LINE_OK);
    t.count = 2;

    assert(find_route(&t, "/api/adder?a=1") == &t.routes[0]);
    assert(find_route(&t, "/img/photo.jpg") == &t.routes[1]);
    assert(find_route(&t, "/index.html") == NULL);
    assert(find_route(&t, "/apix") == NULL);
    assert(find_route(NULL, "/api/x") == NULL);                    /* 규칙 파일 없음 */

    char out[64];
    assert(rewrite_uri(&t.routes[0], "/api/adder?a=1&b=2", out, sizeof(out)) == 0);
    assert(strcmp(out, "/cgi-bin/adder?a=1&b=2") == 0);
    assert(rewrite_uri(&t.routes[1], "/img/photo.jpg", out, sizeof(out)) == 0);
    assert(strcmp(out, "/photo.jpg") == 0);
    char tiny[8];
    assert(rewrite_uri(&t.routes[0], "/api/adder", tiny, sizeof(tiny)) == -1);
}

/* 뒤 서버를 0, 1, 2, 0, 1 ... 순서로 고르는지 확인한다. */
static void test_pick_backend(void)
{
    struct proxy_route r;
    assert(parse_route_line("/a/ /b/ h:1 h:2 h:3", &r) == ROUTE_LINE_OK);
    int expected[] = { 0, 1, 2, 0, 1 };
    for (int i = 0; i < 5; i++)
        assert(pick_backend(&r) == expected[i]);
}

/* 뒤 서버에 보낼 요청: 바꾼 URI, 홉 헤더 제거, 나머지 헤더 유지, Connection: close 추가. */
static void test_build_backend_request(void)
{
    struct http_request req;
    assert(parse_request_line("POST /api/adder HTTP/1.1", &req) == 0);
    assert(parse_header_line("Host: localhost:8080", &req) == 0);
    assert(parse_header_line("Connection: keep-alive", &req) == 0);
    assert(parse_header_line("Content-Length: 7", &req) == 0);
    assert(parse_header_line("Expect: 100-continue", &req) == 0);

    char out[512];
    int n = build_backend_request(&req, "/cgi-bin/adder", out, sizeof(out));
    const char *expected =
        "POST /cgi-bin/adder HTTP/1.1\r\n"
        "Host: localhost:8080\r\n"
        "Content-Length: 7\r\n"
        "Connection: close\r\n"
        "\r\n";
    assert(n == (int)strlen(expected));
    assert(strcmp(out, expected) == 0);

    char small[20];
    assert(build_backend_request(&req, "/cgi-bin/adder", small, sizeof(small)) == -1);
}

/* "forward ..." 줄로 포워드 프록시가 켜지고, 허용 호스트만 통과하는지 확인한다. */
static void test_forward_line(void)
{
    struct proxy_table t;
    memset(&t, 0, sizeof(t));
    assert(is_forward_allowed(&t, "127.0.0.1") == 0);                      /* 기본은 꺼짐 */
    assert(parse_forward_line("/api/ /cgi-bin/ h:1\n", &t) == 0);          /* 다른 규칙 줄 */
    assert(parse_forward_line("forwarder x\n", &t) == 0);                  /* 비슷한 단어 */
    assert(parse_forward_line("forward\n", &t) == 0);                      /* 뒤에 칸이 없음 */
    assert(parse_forward_line("forward   \n", &t) == -1);                  /* 허용 호스트 없음 */
    assert(t.forward_enabled == 0);

    assert(parse_forward_line("forward 127.0.0.1 LocalHost  # 주석\n", &t) == 1);
    assert(t.forward_enabled == 1 && t.forward_host_count == 2);
    assert(is_forward_allowed(&t, "127.0.0.1") == 1);
    assert(is_forward_allowed(&t, "localhost") == 1);                      /* 대소문자 무시 */
    assert(is_forward_allowed(&t, "example.com") == 0);
    assert(is_forward_allowed(NULL, "127.0.0.1") == 0);
}

/* 전체 주소를 호스트·포트·경로로 나누고, 틀린 주소와 https를 구분하는지 확인한다. */
static void test_parse_absolute_uri(void)
{
    char host[64], port[8], path[64];

    assert(is_absolute_uri("http://x/") == 1);
    assert(is_absolute_uri("HTTPS://x/") == 1);
    assert(is_absolute_uri("/index.html") == 0);

    assert(parse_absolute_uri("http://127.0.0.1:9001/a?b=1", host, sizeof(host), port, sizeof(port),
                              path, sizeof(path)) == 0);
    assert(strcmp(host, "127.0.0.1") == 0 && strcmp(port, "9001") == 0 && strcmp(path, "/a?b=1") == 0);

    assert(parse_absolute_uri("http://localhost", host, sizeof(host), port, sizeof(port), path,
                              sizeof(path)) == 0);
    assert(strcmp(host, "localhost") == 0 && strcmp(port, "80") == 0 && strcmp(path, "/") == 0);

    assert(parse_absolute_uri("http://h?x=1", host, sizeof(host), port, sizeof(port), path,
                              sizeof(path)) == 0);
    assert(strcmp(path, "/?x=1") == 0);

    assert(parse_absolute_uri("https://h/", host, sizeof(host), port, sizeof(port), path,
                              sizeof(path)) == ABSOLUTE_URI_HTTPS);
    const char *bad[] = { "http:///a", "http://:80/", "http://h:/", "http://h:8x/",
                          "http://user@h/", "http://[::1]/", "ftp://h/" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        assert(parse_absolute_uri(bad[i], host, sizeof(host), port, sizeof(port), path,
                                  sizeof(path)) == ABSOLUTE_URI_BAD);
}

int main(void)
{
    test_parse_route_line();
    test_find_and_rewrite();
    test_pick_backend();
    test_build_backend_request();
    test_forward_line();
    test_parse_absolute_uri();
    printf("all proxy unit tests passed\n");
    return 0;
}
