#ifndef CONN_H
#define CONN_H

/* 연결 하나를 맡아 요청을 반복해서 처리한다 (keep-alive). */

#define KEEP_ALIVE_TIMEOUT_SEC 5 /* 다음 요청을 기다리는 최대 시간. 넘으면 연결을 닫는다 */

struct proxy_table;
struct thread_pool;

#define IDLE_CHECK_MS 100 /* 다음 요청을 기다리는 동안 이 간격으로 "기다리는 다른 연결이 있나" 확인한다 */

/* 실행할 때 정해지는 서버 설정. 갈래마다 필요한 위치를 담는다 */
struct server_config {
    const char *static_root;   /* 정적 파일 폴더 (www) */
    const char *cgi_dir;       /* CGI 프로그램 폴더 (cgi-bin) */
    struct proxy_table *routes; /* 프록시 규칙. 설정 파일을 안 주면 NULL */
    struct thread_pool *pool;   /* 기다리는 연결이 있는지 볼 스레드 풀. 없으면 NULL */
};

void serve_connection(int connfd, const struct server_config *config);
void request_shutdown(void);
int is_shutting_down(void);

#endif
