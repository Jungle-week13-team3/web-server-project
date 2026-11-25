#ifndef CGI_H
#define CGI_H

#include <stddef.h>

#include "http.h"

/* CGI: /cgi-bin/ 아래 프로그램을 실행하고, 그 출력을 응답으로 보낸다. */

#define CGI_URI_PREFIX  "/cgi-bin/"
#define CGI_NAME_MAX    256
#define CGI_TYPE_MAX    128
#define CGI_OUTPUT_MAX  (1024 * 1024) /* 프로그램 출력은 1MB까지. 넘으면 500 */

int is_cgi_request(const char *uri);
int parse_cgi_uri(const char *uri, char *name, size_t name_cap, const char **query);
int parse_cgi_output(const char *out, size_t len, char *content_type, size_t type_cap,
                     size_t *body_offset);
int serve_cgi(int connfd, const char *cgi_dir, const struct http_request *req, int keep_alive);

#endif
