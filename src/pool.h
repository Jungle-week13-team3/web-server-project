#ifndef POOL_H
#define POOL_H

#include <pthread.h>

/* 스레드 풀: main이 받은 연결을 작업 큐에 넣으면, 미리 만든 작업 스레드들이 꺼내서 처리한다. */

#define POOL_THREADS    8  /* 작업 스레드 수 = 동시에 처리하는 연결 수 */
#define POOL_QUEUE_SIZE 64 /* 처리 대기 중인 연결을 담는 칸 수 */
#define POOL_IP_MAX     16 /* "255.255.255.255" + '\0' */

/* 큐에 들어가는 일감 하나: 연결 소켓과 로그용 클라 주소 */
struct pool_job {
    int connfd;
    char ip[POOL_IP_MAX];
    int port;
};

/* 작업 스레드가 일감 하나를 받으면 부르는 함수 */
typedef void (*pool_handler)(const struct pool_job *job, void *arg);

/* 원형 큐(고정 크기 배열을 빙빙 돌려 쓰는 대기줄)와 그것을 지키는 잠금 */
struct thread_pool {
    struct pool_job jobs[POOL_QUEUE_SIZE];
    int head;  /* 다음에 꺼낼 칸 */
    int tail;  /* 다음에 넣을 칸 */
    int count; /* 지금 들어 있는 일감 수 */
    pthread_mutex_t lock;      /* 큐를 한 번에 한 스레드만 만지게 한다 */
    pthread_cond_t not_empty;  /* "일감이 생겼다" 알림. 작업 스레드가 기다린다 */
    pthread_cond_t not_full;   /* "자리가 생겼다" 알림. main이 기다린다 */
    pthread_t threads[POOL_THREADS];
    int thread_count; /* 실제로 띄운 스레드 수 (종료 때 이만큼 기다린다) */
    int stopping;     /* 1이면 새 일감 없음. 큐에 남은 것까지만 처리하고 스레드가 끝난다 */
    pool_handler handler;
    void *arg;
};

int pool_start(struct thread_pool *pool, pool_handler handler, void *arg);
void pool_submit(struct thread_pool *pool, const struct pool_job *job);
int pool_take(struct thread_pool *pool, struct pool_job *job);
void pool_stop(struct thread_pool *pool);
int pool_has_waiting(struct thread_pool *pool);

#endif
