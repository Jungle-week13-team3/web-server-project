/* static.c의 순수 함수(MIME 결정, 경로 만들기)를 소켓·파일 없이 확인하는 단위 테스트. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "static.h"

/* 확장자별 Content-Type과 기본값을 확인한다. */
static void test_get_mime_type(void)
{
    assert(strcmp(get_mime_type("www/index.html"), "text/html") == 0);
    assert(strcmp(get_mime_type("www/style.css"), "text/css") == 0);
    assert(strcmp(get_mime_type("www/app.js"), "application/javascript") == 0);
    assert(strcmp(get_mime_type("www/image.png"), "image/png") == 0);
    assert(strcmp(get_mime_type("www/photo.jpg"), "image/jpeg") == 0);
    assert(strcmp(get_mime_type("www/photo.jpeg"), "image/jpeg") == 0);
    assert(strcmp(get_mime_type("www/PHOTO.JPG"), "image/jpeg") == 0);    /* 대소문자 무시 */
    assert(strcmp(get_mime_type("www/readme"), "application/octet-stream") == 0);   /* 확장자 없음 */
    assert(strcmp(get_mime_type("www/a.dir/readme"), "application/octet-stream") == 0); /* 폴더의 점 */
    assert(strcmp(get_mime_type("www/archive.zip"), "application/octet-stream") == 0);  /* 표에 없음 */
}

/* URI → 파일 경로 변환, 쿼리 제거, index.html, ".." 차단, 길이 초과를 확인한다. */
static void test_build_static_path(void)
{
    char out[64];

    assert(build_static_path("www", "/index.html", out, sizeof(out)) == 0);
    assert(strcmp(out, "www/index.html") == 0);
    assert(build_static_path("www", "/", out, sizeof(out)) == 0);
    assert(strcmp(out, "www/index.html") == 0);
    assert(build_static_path("www", "/sub/", out, sizeof(out)) == 0);
    assert(strcmp(out, "www/sub/index.html") == 0);
    assert(build_static_path("www", "/a.css?v=2", out, sizeof(out)) == 0);  /* 쿼리는 버린다 */
    assert(strcmp(out, "www/a.css") == 0);
    assert(build_static_path("www", "/..foo", out, sizeof(out)) == 0);      /* ".."이 칸 전체가 아니면 괜찮다 */

    assert(build_static_path("www", "/../etc/passwd", out, sizeof(out)) == STATIC_PATH_OUTSIDE);
    assert(build_static_path("www", "/a/../../x", out, sizeof(out)) == STATIC_PATH_OUTSIDE);
    assert(build_static_path("www", "/a/..", out, sizeof(out)) == STATIC_PATH_OUTSIDE);
    assert(build_static_path("www", "/..", out, sizeof(out)) == STATIC_PATH_OUTSIDE);

    char tiny[8];
    assert(build_static_path("www", "/index.html", tiny, sizeof(tiny)) == STATIC_PATH_TOO_LONG);
}

int main(void)
{
    test_get_mime_type();
    test_build_static_path();
    printf("all static unit tests passed\n");
    return 0;
}
