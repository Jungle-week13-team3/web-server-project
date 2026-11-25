#include "http.h"

#include <stdio.h>
#include <string.h>
#include <strings.h> /* strcasecmp: 대소문자를 무시하고 비교 (POSIX) */

/* src의 len글자를 dst(크기 cap)에 복사하고 '\0'을 붙인다. 자리가 모자라면 -1. */
static int copy_token(char *dst, size_t cap, const char *src, size_t len)
{
    if (len == 0 || len >= cap)
        return -1;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return 0;
}

/* 요청 줄 "GET /index.html HTTP/1.1"을 메서드·URI·버전으로 나눠 req에 넣는다.
 * req의 헤더 목록도 비운다. 맞으면 0, 형식이 틀리면 PARSE_BAD_REQUEST, URI가 너무 길면 PARSE_URI_TOO_LONG. */
int parse_request_line(const char *line, struct http_request *req)
{
    memset(req, 0, sizeof(*req));

    /* 칸은 공백 하나로 나뉜다. 공백이 정확히 2개여야 칸이 3개다 */
    const char *sp1 = strchr(line, ' ');
    if (sp1 == NULL)
        return -1;
    const char *sp2 = strchr(sp1 + 1, ' ');
    if (sp2 == NULL || strchr(sp2 + 1, ' ') != NULL)
        return -1;

    /* URI가 길어서 실패한 것만 따로 알려 줘야 400 대신 414로 답할 수 있다 */
    if ((size_t)(sp2 - sp1 - 1) >= sizeof(req->uri))
        return PARSE_URI_TOO_LONG;

    if (copy_token(req->method, sizeof(req->method), line, (size_t)(sp1 - line)) < 0 ||
        copy_token(req->uri, sizeof(req->uri), sp1 + 1, (size_t)(sp2 - sp1 - 1)) < 0 ||
        copy_token(req->version, sizeof(req->version), sp2 + 1, strlen(sp2 + 1)) < 0)
        return -1;

    /* URI는 세 가지 모양만 받는다.
     * 경로 "/index.html": 보통 요청
     * 전체 주소 "http://host:port/path": 포워드 프록시에게 보내는 요청
     * CONNECT의 "host:port": https 터널 요청 (지원하지 않지만 501로 답하려면 형식은 통과시킨다) */
    int is_path = req->uri[0] == '/';
    int is_absolute = strncasecmp(req->uri, "http://", 7) == 0 ||
                      strncasecmp(req->uri, "https://", 8) == 0;
    int is_connect = strcmp(req->method, "CONNECT") == 0;
    if (!is_path && !is_absolute && !is_connect)
        return -1;
    /* 버전은 "HTTP/"로 시작해야 한다. 지원하는 버전인지는 decide_request_status가 본다 */
    if (strncmp(req->version, "HTTP/", 5) != 0)
        return -1;
    return 0;
}

/* 헤더 한 줄 "Name: value"를 나눠 req의 헤더 목록 끝에 붙인다.
 * ':'가 없거나, 이름이 비었거나 공백이 있거나, 너무 길거나, 헤더가 너무 많으면 -1. */
int parse_header_line(const char *line, struct http_request *req)
{
    if (req->header_count >= HTTP_MAX_HEADERS)
        return -1;

    const char *colon = strchr(line, ':');
    if (colon == NULL)
        return -1;

    /* 이름에는 공백·탭이 들어갈 수 없다 ("Host : x"는 잘못된 헤더) */
    size_t name_len = (size_t)(colon - line);
    for (size_t i = 0; i < name_len; i++) {
        if (line[i] == ' ' || line[i] == '\t')
            return -1;
    }

    /* 값의 앞뒤 공백·탭은 떼어 낸다 */
    const char *value = colon + 1;
    while (*value == ' ' || *value == '\t')
        value++;
    size_t value_len = strlen(value);
    while (value_len > 0 && (value[value_len - 1] == ' ' || value[value_len - 1] == '\t'))
        value_len--;

    struct http_header *h = &req->headers[req->header_count];
    if (copy_token(h->name, sizeof(h->name), line, name_len) < 0)
        return -1;
    /* 값은 비어 있어도 된다 ("X-Empty:"). 그래서 copy_token 대신 직접 복사한다 */
    if (value_len >= sizeof(h->value))
        return -1;
    memcpy(h->value, value, value_len);
    h->value[value_len] = '\0';

    req->header_count++;
    return 0;
}

/* 이름이 name인 첫 헤더의 값을 돌려준다. 없으면 NULL. 이름은 대소문자를 가리지 않는다. */
const char *find_header(const struct http_request *req, const char *name)
{
    for (int i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].name, name) == 0)
            return req->headers[i].value;
    }
    return NULL;
}

/* 이름이 name인 헤더가 몇 개인지 센다. */
static int count_header(const struct http_request *req, const char *name)
{
    int count = 0;
    for (int i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].name, name) == 0)
            count++;
    }
    return count;
}

/* 해석이 끝난 요청을 보고 어떤 상태 코드로 답할지 정한다 (200·400·501·505). */
int decide_request_status(const struct http_request *req)
{
    /* 메서드는 대소문자를 가린다. "get"은 GET이 아니다 */
    if (strcmp(req->method, "GET") != 0 && strcmp(req->method, "POST") != 0)
        return 501;
    /* 본문을 조각(chunked)으로 보내는 방식은 지원하지 않는다. 본문 길이는 Content-Length로만 안다 */
    if (find_header(req, "Transfer-Encoding") != NULL)
        return 501;

    int is_http11 = strcmp(req->version, "HTTP/1.1") == 0;
    if (!is_http11 && strcmp(req->version, "HTTP/1.0") != 0)
        return 505;

    /* HTTP/1.1은 Host 헤더가 정확히 하나 있어야 한다 */
    if (is_http11 && count_header(req, "Host") != 1)
        return 400;
    return 200;
}

/* Content-Length 헤더를 숫자로 바꿔 *out에 넣는다. 헤더가 없으면 본문이 없다는 뜻이라 0.
 * 성공 0, 숫자가 아니거나 두 번 이상 나오면 CONTENT_LENGTH_INVALID, 너무 크면 CONTENT_LENGTH_TOO_LARGE. */
int get_content_length(const struct http_request *req, size_t *out)
{
    *out = 0;
    int count = count_header(req, "Content-Length");
    if (count == 0)
        return 0;
    if (count > 1) /* 길이가 두 개면 어느 쪽이 맞는지 알 수 없다 */
        return CONTENT_LENGTH_INVALID;

    const char *value = find_header(req, "Content-Length");
    if (*value == '\0')
        return CONTENT_LENGTH_INVALID;
    size_t len = 0;
    for (const char *p = value; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') /* "+5", "-1", "5 bytes" 같은 건 받지 않는다 */
            return CONTENT_LENGTH_INVALID;
        len = len * 10 + (size_t)(*p - '0');
        if (len > HTTP_MAX_BODY) /* 계속 곱하면 넘칠 수 있으니 상한을 넘는 순간 멈춘다 */
            return CONTENT_LENGTH_TOO_LARGE;
    }
    *out = len;
    return 0;
}

/* 응답 뒤에도 연결을 유지할지 정한다. 1이면 유지, 0이면 닫는다.
 * HTTP/1.1은 "Connection: close"가 없으면 유지, HTTP/1.0은 "Connection: keep-alive"가 있어야 유지. */
int wants_keep_alive(const struct http_request *req)
{
    const char *conn = find_header(req, "Connection");
    if (strcmp(req->version, "HTTP/1.1") == 0)
        return !(conn != NULL && strcasecmp(conn, "close") == 0);
    return conn != NULL && strcasecmp(conn, "keep-alive") == 0;
}

/* 상태 코드에 맞는 설명 문구를 돌려준다. 예: 404 → "Not Found" */
const char *get_status_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

/* 상태 줄과 헤더(빈 줄까지)를 out에 만든다. 만든 길이를 돌려주고, out이 모자라면 -1.
 * keep_alive가 1이면 "Connection: keep-alive", 0이면 "Connection: close"를 쓴다. */
int build_response_head(char *out, size_t cap, int status, const char *content_type,
                        size_t content_length, int keep_alive)
{
    int n = snprintf(out, cap,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: %s\r\n"
                     "\r\n",
                     status, get_status_reason(status), content_type, content_length,
                     keep_alive ? "keep-alive" : "close");
    /* snprintf는 잘렸어도 "원래 필요했던 길이"를 돌려주므로 cap과 비교해 잘림을 잡는다 */
    if (n < 0 || (size_t)n >= cap)
        return -1;
    return n;
}
