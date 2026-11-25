#include "static.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h> /* strcasecmp */
#include <sys/stat.h>
#include <unistd.h>

#include "http.h"
#include "net.h"

#define STATIC_HEAD_MAX 512
#define STATIC_COPY_BUF 8192

/* 확장자 → Content-Type 표. 서버는 내용을 해석하지 않고 이 이름표만 붙인다. */
static const struct {
    const char *ext;
    const char *type;
} mime_table[] = {
    { ".html", "text/html" },
    { ".htm",  "text/html" },
    { ".css",  "text/css" },
    { ".js",   "application/javascript" },
    { ".png",  "image/png" },
    { ".jpg",  "image/jpeg" },
    { ".jpeg", "image/jpeg" },
    { ".txt",  "text/plain" },
};

/* 파일 경로의 확장자를 보고 Content-Type을 돌려준다. 표에 없으면 "그냥 바이트"라는 뜻의 기본값. */
const char *get_mime_type(const char *path)
{
    /* 마지막 '/' 뒤에서 마지막 '.'을 찾아야 "a.d/file" 같은 폴더 이름의 점에 속지 않는다 */
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    if (dot != NULL) {
        for (size_t i = 0; i < sizeof(mime_table) / sizeof(mime_table[0]); i++) {
            if (strcasecmp(dot, mime_table[i].ext) == 0)
                return mime_table[i].type;
        }
    }
    return "application/octet-stream";
}

/* URI 경로 부분(첫 len글자)에 ".." 칸이 하나라도 있으면 1. "/a/../b", "/..", "/../x" 모두 해당. */
static int has_dot_dot_segment(const char *uri, size_t len)
{
    size_t i = 0;
    while (i < len) {
        while (i < len && uri[i] == '/')
            i++;
        size_t start = i;
        while (i < len && uri[i] != '/')
            i++;
        if (i - start == 2 && uri[start] == '.' && uri[start + 1] == '.')
            return 1;
    }
    return 0;
}

/* URI "/index.html?x=1"을 루트 안의 파일 경로 "www/index.html"로 바꿔 out에 쓴다.
 * '?' 뒤(쿼리)는 버리고, '/'로 끝나면 index.html을 붙인다. 성공 0, 실패는 STATIC_PATH_* 값. */
int build_static_path(const char *root, const char *uri, char *out, size_t cap)
{
    const char *query = strchr(uri, '?');
    size_t path_len = query ? (size_t)(query - uri) : strlen(uri);

    /* "www/" + "/../etc/passwd" = "www/../etc/passwd" = 루트 밖. 그래서 ".." 칸은 아예 받지 않는다 */
    if (has_dot_dot_segment(uri, path_len))
        return STATIC_PATH_OUTSIDE;

    const char *index = (path_len > 0 && uri[path_len - 1] == '/') ? "index.html" : "";
    int n = snprintf(out, cap, "%s%.*s%s", root, (int)path_len, uri, index);
    if (n < 0 || (size_t)n >= cap)
        return STATIC_PATH_TOO_LONG;
    return 0;
}

/* URI에 해당하는 파일을 찾아 200 응답(머리 + 파일 바이트)을 보낸다.
 * 보낼 수 없으면 아무것도 보내지 않고 403·404·500 중 하나를 돌려준다. 에러 응답은 부른 쪽이 보낸다.
 * keep_alive는 응답 머리의 Connection 값에 쓴다. */
int serve_static(int connfd, const char *root, const char *uri, int keep_alive)
{
    char path[STATIC_PATH_MAX];
    int rc = build_static_path(root, uri, path, sizeof(path));
    if (rc == STATIC_PATH_OUTSIDE)
        return 403;
    if (rc < 0)
        return 500;

    /* 머리를 보내기 전에 열어 봐야 실패를 상태 코드로 알릴 수 있다 */
    int fd = open(path, O_RDONLY | O_CLOEXEC); /* 다른 스레드가 돌린 CGI에 물려주지 않는다 */
    if (fd < 0) {
        if (errno == ENOENT || errno == ENOTDIR)
            return 404; /* 없는 파일 */
        if (errno == EACCES)
            return 403; /* 읽기 권한 없음 */
        return 500;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return 500;
    }
    if (!S_ISREG(st.st_mode)) { /* 폴더 같은 일반 파일이 아닌 것은 내주지 않는다 */
        close(fd);
        return 403;
    }

    char head[STATIC_HEAD_MAX];
    int head_len = build_response_head(head, sizeof(head), 200, get_mime_type(path),
                                       (size_t)st.st_size, keep_alive);
    if (head_len < 0) {
        close(fd);
        return 500;
    }
    if (write_all(connfd, head, (size_t)head_len) < 0) {
        perror("write");
        close(fd);
        return 200; /* 머리는 이미 나가기 시작했으니 상태를 바꿀 수 없다 */
    }

    /* 본문: 파일을 조각씩 읽어 바이트 그대로 보낸다. 이미지도 있으니 줄 단위로 읽지 않는다 */
    char buf[STATIC_COPY_BUF];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("read file"); /* 머리가 나간 뒤라 500을 보낼 수 없다. 클라는 길이가 모자란 걸로 안다 */
            break;
        }
        if (n == 0)
            break;
        if (write_all(connfd, buf, (size_t)n) < 0) {
            perror("write");
            break;
        }
    }
    close(fd);
    return 200;
}
