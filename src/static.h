#ifndef STATIC_H
#define STATIC_H

#include <stddef.h>

/* 정적 파일 응답: URI를 루트 폴더 안의 파일 경로로 바꾸고, 그 파일을 그대로 보낸다. */

#define STATIC_PATH_MAX 8192

/* build_static_path가 0(성공) 대신 돌려주는 값 */
#define STATIC_PATH_OUTSIDE  (-1) /* ".." 때문에 루트 밖으로 나갈 수 있는 경로 → 403 */
#define STATIC_PATH_TOO_LONG (-2) /* 만든 경로가 out에 안 들어간다 → 500 */

const char *get_mime_type(const char *path);
int build_static_path(const char *root, const char *uri, char *out, size_t cap);
int serve_static(int connfd, const char *root, const char *uri, int keep_alive);

#endif
