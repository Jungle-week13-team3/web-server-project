/* cgi.c의 순수 함수(URI 나누기, 출력 해석)를 프로세스 실행 없이 확인하는 단위 테스트. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cgi.h"

/* /cgi-bin/ 접두어를 알아보는지 확인한다. */
static void test_is_cgi_request(void)
{
    assert(is_cgi_request("/cgi-bin/adder") == 1);
    assert(is_cgi_request("/cgi-bin/adder?a=1") == 1);
    assert(is_cgi_request("/index.html") == 0);
    assert(is_cgi_request("/cgi-binx/adder") == 0);
    assert(is_cgi_request("/cgi-bin") == 0);
}

/* 프로그램 이름과 쿼리를 나누고, 위험하거나 빈 이름을 거절하는지 확인한다. */
static void test_parse_cgi_uri(void)
{
    char name[32];
    const char *query;

    assert(parse_cgi_uri("/cgi-bin/adder?a=3&b=4", name, sizeof(name), &query) == 0);
    assert(strcmp(name, "adder") == 0);
    assert(strcmp(query, "a=3&b=4") == 0);

    assert(parse_cgi_uri("/cgi-bin/adder", name, sizeof(name), &query) == 0);
    assert(strcmp(query, "") == 0);                                         /* 쿼리 없음 */

    assert(parse_cgi_uri("/cgi-bin/", name, sizeof(name), &query) == -1);       /* 이름 없음 */
    assert(parse_cgi_uri("/cgi-bin/?a=1", name, sizeof(name), &query) == -1);
    assert(parse_cgi_uri("/cgi-bin/../server", name, sizeof(name), &query) == -1); /* 폴더 밖 */
    assert(parse_cgi_uri("/cgi-bin/sub/x", name, sizeof(name), &query) == -1);  /* 하위 폴더 */
    assert(parse_cgi_uri("/cgi-bin/..", name, sizeof(name), &query) == -1);
    assert(parse_cgi_uri("/cgi-bin/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", name, sizeof(name),
                         &query) == -1);                                    /* 이름이 너무 김 */
}

/* 프로그램 출력에서 Content-Type과 본문 시작 위치를 찾는지 확인한다. */
static void test_parse_cgi_output(void)
{
    char type[64];
    size_t off;

    const char *out1 = "Content-Type: text/html\r\n\r\n<b>7</b>";
    assert(parse_cgi_output(out1, strlen(out1), type, sizeof(type), &off) == 0);
    assert(strcmp(type, "text/html") == 0);
    assert(strcmp(out1 + off, "<b>7</b>") == 0);

    const char *out2 = "X-Other: 1\n\n7\n";                    /* '\n'만 써도 된다. 타입 없으면 text/plain */
    assert(parse_cgi_output(out2, strlen(out2), type, sizeof(type), &off) == 0);
    assert(strcmp(type, "text/plain") == 0);
    assert(strcmp(out2 + off, "7\n") == 0);

    const char *out3 = "content-type: image/png\r\n\r\n";      /* 이름 대소문자 무시, 빈 본문 */
    assert(parse_cgi_output(out3, strlen(out3), type, sizeof(type), &off) == 0);
    assert(strcmp(type, "image/png") == 0 && off == strlen(out3));

    const char *bad1 = "7\n";                                  /* 빈 줄이 없다 */
    assert(parse_cgi_output(bad1, strlen(bad1), type, sizeof(type), &off) == -1);
    assert(parse_cgi_output("", 0, type, sizeof(type), &off) == -1);       /* 출력 없음 */
    const char *bad2 = "Content-Type: text/plain";             /* 줄이 끝나지 않음 */
    assert(parse_cgi_output(bad2, strlen(bad2), type, sizeof(type), &off) == -1);
}

int main(void)
{
    test_is_cgi_request();
    test_parse_cgi_uri();
    test_parse_cgi_output();
    printf("all cgi unit tests passed\n");
    return 0;
}
