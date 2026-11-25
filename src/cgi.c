#define _GNU_SOURCE /* pipe2: 리눅스 확장. 모든 #include보다 먼저 와야 한다 */
#include "cgi.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strncasecmp */
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "net.h"

#define CGI_HEAD_MAX 512

/* URI가 "/cgi-bin/"으로 시작하면 1. 갈림길에서 CGI로 보낼지 정할 때 쓴다. */
int is_cgi_request(const char *uri)
{
    return strncmp(uri, CGI_URI_PREFIX, strlen(CGI_URI_PREFIX)) == 0;
}

/* "/cgi-bin/adder?a=3&b=4"를 프로그램 이름 "adder"와 입력 "a=3&b=4"로 나눈다.
 * *query는 uri 안의 '?' 다음 위치를 가리킨다(없으면 빈 문자열). 성공 0.
 * 이름이 비었거나, '/'가 들어 있거나(하위 폴더·".." 차단), 너무 길면 -1. */
int parse_cgi_uri(const char *uri, char *name, size_t name_cap, const char **query)
{
    const char *start = uri + strlen(CGI_URI_PREFIX);
    const char *q = strchr(start, '?');
    size_t len = q ? (size_t)(q - start) : strlen(start);

    if (len == 0 || len >= name_cap)
        return -1;
    if (memchr(start, '/', len) != NULL)
        return -1;
    if (len == 2 && start[0] == '.' && start[1] == '.')
        return -1;

    memcpy(name, start, len);
    name[len] = '\0';
    *query = q ? q + 1 : "";
    return 0;
}

/* 프로그램 출력 "Content-Type: text/plain\r\n\r\n7\n"을 머리와 본문으로 나눈다.
 * CGI 약속: 프로그램은 헤더 줄들, 빈 줄, 본문 순서로 출력한다.
 * Content-Type을 content_type에 넣고(없으면 text/plain), 본문 시작 위치를 *body_offset에 넣는다.
 * 빈 줄이 없거나 Content-Type 값이 너무 길면 -1. */
int parse_cgi_output(const char *out, size_t len, char *content_type, size_t type_cap,
                     size_t *body_offset)
{
    snprintf(content_type, type_cap, "text/plain");
    size_t pos = 0;
    while (pos < len) {
        const char *nl = memchr(out + pos, '\n', len - pos);
        if (nl == NULL)
            return -1; /* 줄이 끝나지 않았다 = 빈 줄을 못 만났다 */
        size_t line_len = (size_t)(nl - (out + pos));
        if (line_len > 0 && out[pos + line_len - 1] == '\r')
            line_len--;

        if (line_len == 0) { /* 빈 줄 = 머리 끝. 그 다음부터 본문 */
            *body_offset = (size_t)(nl - out) + 1;
            return 0;
        }

        const char *line = out + pos;
        const char *key = "Content-Type:";
        size_t key_len = strlen(key);
        if (line_len > key_len && strncasecmp(line, key, key_len) == 0) {
            const char *v = line + key_len;
            size_t v_len = line_len - key_len;
            while (v_len > 0 && (*v == ' ' || *v == '\t')) {
                v++;
                v_len--;
            }
            if (v_len == 0 || v_len >= type_cap)
                return -1;
            memcpy(content_type, v, v_len);
            content_type[v_len] = '\0';
        }
        /* 다른 헤더는 지금은 쓰지 않고 넘긴다 */
        pos = (size_t)(nl - out) + 1;
    }
    return -1;
}

/* CGI 프로그램에 넘길 환경변수 문자열들. fork 전에 부모가 다 만들어 둔다 */
struct cgi_env {
    char method[32 + HTTP_METHOD_MAX];
    char query[32 + HTTP_URI_MAX];
    char length[64];
    char path[16 + 1024];
    char *envp[5]; /* 위 문자열들을 가리키는 목록. 끝은 NULL */
};

/* [부모] 요청 정보로 "REQUEST_METHOD=GET" 같은 환경변수 목록을 env에 만든다. */
static void build_cgi_env(struct cgi_env *env, const struct http_request *req, const char *query)
{
    const char *path = getenv("PATH");
    snprintf(env->method, sizeof(env->method), "REQUEST_METHOD=%s", req->method);
    snprintf(env->query, sizeof(env->query), "QUERY_STRING=%s", query);
    snprintf(env->length, sizeof(env->length), "CONTENT_LENGTH=%zu", req->body_len);
    snprintf(env->path, sizeof(env->path), "PATH=%s", path ? path : "/usr/bin:/bin");
    env->envp[0] = env->method;
    env->envp[1] = env->query;
    env->envp[2] = env->length;
    env->envp[3] = env->path;
    env->envp[4] = NULL;
}

/* [자식 프로세스] 표준입력·출력을 파이프에 연결하고 프로그램으로 바뀐다.
 * 스레드가 여럿인 프로그램에서 fork한 자식은 malloc 같은 함수를 부르면 멈출 수 있다
 * (다른 스레드가 쥐고 있던 잠금이 풀리지 않은 채 복사된다). 그래서 여기선 dup2·signal·execve·_exit만 쓴다.
 * execve가 성공하면 돌아오지 않는다. 실패하면 종료 코드 127로 끝난다. */
static void run_cgi_child(const char *path, char *const envp[], int in_fd, int out_fd)
{
    dup2(in_fd, STDIN_FILENO);   /* 프로그램이 표준입력을 읽으면 = 부모가 넘긴 POST 본문 */
    dup2(out_fd, STDOUT_FILENO); /* 프로그램이 표준출력에 쓰면 = 부모에게 간다 */
    /* 파이프 원래 번호와 클라 소켓은 CLOEXEC라서 execve 때 저절로 닫힌다. dup2로 만든 0·1번은 남는다 */

    /* 부모(서버)가 무시하던 SIGPIPE를 원래대로 돌린다. 프로그램은 보통의 환경에서 돌아야 한다 */
    signal(SIGPIPE, SIG_DFL);

    char *argv[] = { (char *)path, NULL };
    execve(path, argv, envp);
    _exit(127); /* 여기 왔다면 execve 실패 */
}

/* [부모 프로세스] fd에서 EOF까지 읽어 malloc한 버퍼로 돌려준다. 길이는 *len.
 * 출력이 CGI_OUTPUT_MAX를 넘거나 읽기 오류면 NULL. */
static char *read_all_output(int fd, size_t *len)
{
    size_t cap = 4096, used = 0;
    char *buf = malloc(cap);
    if (buf == NULL)
        return NULL;
    for (;;) {
        if (used == cap) {
            if (cap >= CGI_OUTPUT_MAX) {
                free(buf);
                return NULL;
            }
            cap *= 2;
            char *bigger = realloc(buf, cap);
            if (bigger == NULL) {
                free(buf);
                return NULL;
            }
            buf = bigger;
        }
        ssize_t n = read(fd, buf + used, cap - used);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            return NULL;
        }
        if (n == 0)
            break;
        used += (size_t)n;
    }
    *len = used;
    return buf;
}

/* /cgi-bin/ 요청을 처리한다. 프로그램을 실행하고 출력 전체를 받은 뒤, 머리를 붙여 클라에 보낸다.
 * 보냈으면 200, 못 보냈으면 아무것도 보내지 않고 403·404·500을 돌려준다. 에러 응답은 부른 쪽이 보낸다. */
int serve_cgi(int connfd, const char *cgi_dir, const struct http_request *req, int keep_alive)
{
    /* ① URI → 프로그램 경로와 입력 */
    char name[CGI_NAME_MAX];
    const char *query;
    if (parse_cgi_uri(req->uri, name, sizeof(name), &query) < 0)
        return 404;
    char path[CGI_NAME_MAX + 512];
    int n = snprintf(path, sizeof(path), "%s/%s", cgi_dir, name);
    if (n < 0 || (size_t)n >= sizeof(path))
        return 500;

    /* ② 프로그램 확인: fork하기 전에 걸러야 404·403을 구분해서 답할 수 있다 */
    struct stat st;
    if (stat(path, &st) < 0)
        return (errno == ENOENT || errno == ENOTDIR) ? 404 : 500;
    if (!S_ISREG(st.st_mode) || access(path, X_OK) < 0)
        return 403;

    /* ③ 파이프 두 개: 부모 → 자식 표준입력(본문), 자식 표준출력 → 부모(결과).
     * O_CLOEXEC: 다른 스레드가 동시에 돌린 CGI에 이 파이프가 새면, 그 CGI가 끝날 때까지 EOF가 안 온다 */
    struct cgi_env env;
    build_cgi_env(&env, req, query);
    int in_pipe[2], out_pipe[2];
    if (pipe2(in_pipe, O_CLOEXEC) < 0)
        return 500;
    if (pipe2(out_pipe, O_CLOEXEC) < 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        return 500;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        return 500;
    }
    if (pid == 0)
        run_cgi_child(path, env.envp, in_pipe[0], out_pipe[1]);

    /* 부모: 자식 쪽 끝은 닫는다. 안 닫으면 자식이 끝나도 출력 파이프에서 EOF가 오지 않는다 */
    close(in_pipe[0]);
    close(out_pipe[1]);

    /* 본문을 넘기고 닫는다. 닫아야 프로그램이 "입력 끝"을 안다.
     * 프로그램이 본문을 안 읽고 끝났으면 쓰기가 실패하는데(EPIPE), 출력은 그대로 받으면 된다 */
    if (req->body_len > 0)
        write_all(in_pipe[1], req->body, req->body_len);
    close(in_pipe[1]);

    size_t out_len = 0;
    char *out = read_all_output(out_pipe[0], &out_len);
    close(out_pipe[0]);
    if (out == NULL)
        kill(pid, SIGKILL); /* 출력이 너무 크다: 더 기다리지 않게 끝낸다 */

    /* 자식이 끝나길 기다려 정리한다. 안 하면 끝난 자식이 좀비로 남는다 */
    int wstatus;
    while (waitpid(pid, &wstatus, 0) < 0 && errno == EINTR)
        ;
    if (out == NULL)
        return 500;
    if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
        free(out);
        return 500; /* 프로그램이 실패했다(실행 실패 127 포함) */
    }

    /* ④ 출력 = CGI 머리 + 빈 줄 + 본문. 본문에 우리 머리를 붙여 보낸다 */
    char content_type[CGI_TYPE_MAX];
    size_t body_offset;
    if (parse_cgi_output(out, out_len, content_type, sizeof(content_type), &body_offset) < 0) {
        free(out);
        return 500;
    }
    size_t body_len = out_len - body_offset;
    char head[CGI_HEAD_MAX];
    int head_len = build_response_head(head, sizeof(head), 200, content_type, body_len, keep_alive);
    if (head_len < 0) {
        free(out);
        return 500;
    }
    if (write_all(connfd, head, (size_t)head_len) < 0 ||
        write_all(connfd, out + body_offset, body_len) < 0)
        perror("write");
    free(out);
    return 200;
}
