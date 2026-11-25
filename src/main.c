#define _GNU_SOURCE /* accept4: 리눅스 확장 함수를 쓰겠다는 표시. 모든 #include보다 먼저 와야 한다 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "conn.h"
#include "net.h"
#include "pool.h"
#include "proxy.h"

#define DEFAULT_STATIC_ROOT "www"
#define DEFAULT_CGI_DIR     "cgi-bin"

static struct proxy_table routes; /* 프록시 규칙. 서버가 도는 내내 쓰므로 전역에 둔다 */

/* [작업 스레드] 큐에서 꺼낸 연결 하나를 끝까지 처리하고 닫는다. arg는 서버 설정이다. */
static void handle_pool_job(const struct pool_job *job, void *arg)
{
    const struct server_config *config = arg;
    serve_connection(job->connfd, config); /* keep-alive면 요청 여러 개를 처리하고 돌아온다 */
    drain_and_close(job->connfd); /* close 대신: 안 읽은 입력 때문에 응답이 사라지지 않게 */
    printf("closed: %s:%d\n", job->ip, job->port);
    fflush(stdout);
}

/* 포트, 정적 파일 루트, CGI 폴더, 프록시 규칙 파일을 인자로 받아 듣기 소켓을 열고,
 * 받은 연결을 스레드 풀에 넘겨 동시에 처리한다. */
int main(int argc, char **argv)
{
    // 인자는 포트, 그리고 생략 가능한 정적 파일 루트, CGI 폴더, 프록시 규칙 파일
    if (argc < 2 || argc > 5) {
        fprintf(stderr, "usage: %s <port> [static_root] [cgi_dir] [route_file]\n", argv[0]);
        return 1;
    }
    const char *static_root = (argc >= 3) ? argv[2] : DEFAULT_STATIC_ROOT;
    struct server_config config = {
        .static_root = static_root,
        .cgi_dir = (argc >= 4) ? argv[3] : DEFAULT_CGI_DIR,
        .routes = NULL,
    };

    // 규칙 파일을 줬으면 읽는다. 틀린 줄이 있으면 잘못 중계하기 전에 시작을 막는다
    if (argc == 5) {
        if (load_route_file(argv[4], &routes) < 0)
            return 1;
        config.routes = &routes;
        printf("loaded %d proxy route(s) from %s\n", routes.count, argv[4]);
    }

    // 루트가 폴더가 아니면 모든 요청이 404가 되니 시작할 때 막는다
    struct stat root_st;
    if (stat(static_root, &root_st) < 0 || !S_ISDIR(root_st.st_mode)) {
        fprintf(stderr, "static root is not a directory: %s\n", static_root);
        return 1;
    }

    // 포트번호가 맞는지 확인
    char *end;
    long port = strtol(argv[1], &end, 10);
    if (*end != '\0' || port < 1 || port > 65535) {
        fprintf(stderr, "invalid port: %s\n", argv[1]);
        return 1;
    }

    /* 클라가 먼저 끊은 연결에 write하면 OS가 SIGPIPE를 보내고, 기본 동작은 프로세스 종료다.
     * 무시하면 write가 -1(EPIPE)을 돌려주고, write_all이 그걸 실패로 처리한다 */
    signal(SIGPIPE, SIG_IGN);

    /* getaddrinfo는 포트를 문자열로 받으므로 검사한 argv[1]을 그대로 넘긴다 */
    int listenfd = open_listen_socket(argv[1]);
    if (listenfd < 0)
        return 1;
    /* 작업 스레드들을 먼저 띄운다. 이제 main은 받기만 하고, 처리는 작업 스레드가 한다 */
    static struct thread_pool pool; /* 큐가 커서 스택 대신 전역 영역에 둔다 */
    if (pool_start(&pool, handle_pool_job, &config) < 0)
        return 1;

    printf("listening on port %ld, static root %s, cgi dir %s, %d threads\n", port, static_root,
           config.cgi_dir, POOL_THREADS);
    fflush(stdout);

    for (;;) {
        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        /* SOCK_CLOEXEC: 이 소켓은 CGI 프로그램(execve)에 물려주지 않는다.
         * 물려주면 다른 스레드가 돌린 CGI가 끝날 때까지 이 연결이 안 닫힌다 */
        int connfd = accept4(listenfd, (struct sockaddr *)&cliaddr, &clilen, SOCK_CLOEXEC);
        if (connfd < 0) {
            if (errno != EINTR)
                perror("accept");
            continue;
        }

        struct pool_job job = { .connfd = connfd, .port = ntohs(cliaddr.sin_port) };
        inet_ntop(AF_INET, &cliaddr.sin_addr, job.ip, sizeof(job.ip));
        printf("connected: %s:%d\n", job.ip, job.port);
        fflush(stdout);

        pool_submit(&pool, &job); /* 큐가 꽉 찼으면 자리가 날 때까지 여기서 기다린다 */
    }
}
