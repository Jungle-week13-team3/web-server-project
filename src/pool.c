#include "pool.h"

#include <stdio.h>
#include <string.h>

/* 작업 스레드 본체: 큐에서 일감을 꺼내 처리하기를 영원히 반복한다. 일감이 없으면 잠든다. */
static void *run_worker(void *arg)
{
    struct thread_pool *pool = arg;
    for (;;) {
        struct pool_job job;
        pool_take(pool, &job);
        pool->handler(&job, pool->arg);
    }
    return NULL;
}

/* 큐와 잠금을 준비하고 작업 스레드 POOL_THREADS개를 띄운다. 일감마다 handler(job, arg)를 부른다.
 * 성공 0, 스레드를 못 만들면 -1. */
int pool_start(struct thread_pool *pool, pool_handler handler, void *arg)
{
    memset(pool, 0, sizeof(*pool));
    pool->handler = handler;
    pool->arg = arg;
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->not_empty, NULL);
    pthread_cond_init(&pool->not_full, NULL);

    for (int i = 0; i < POOL_THREADS; i++) {
        int rc = pthread_create(&pool->threads[i], NULL, run_worker, pool);
        if (rc != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(rc));
            return -1;
        }
        pthread_detach(pool->threads[i]); /* 끝을 기다릴 일이 없으니 끝나면 알아서 정리되게 한다 */
    }
    return 0;
}

/* [main] 일감을 큐 끝에 넣고 작업 스레드 하나를 깨운다. 큐가 꽉 찼으면 자리가 날 때까지 기다린다.
 * 기다리는 동안 새 접속은 OS의 접속 대기줄(listen backlog)에 쌓인다. */
void pool_submit(struct thread_pool *pool, const struct pool_job *job)
{
    pthread_mutex_lock(&pool->lock);
    while (pool->count == POOL_QUEUE_SIZE) /* 깨어났어도 다른 스레드가 먼저 채웠을 수 있어 다시 확인한다 */
        pthread_cond_wait(&pool->not_full, &pool->lock);

    pool->jobs[pool->tail] = *job;
    pool->tail = (pool->tail + 1) % POOL_QUEUE_SIZE;
    pool->count++;

    pthread_cond_signal(&pool->not_empty);
    pthread_mutex_unlock(&pool->lock);
}

/* [작업 스레드] 큐 앞에서 일감 하나를 꺼내 job에 넣는다. 큐가 비었으면 일감이 올 때까지 잠든다. 항상 0. */
int pool_take(struct thread_pool *pool, struct pool_job *job)
{
    pthread_mutex_lock(&pool->lock);
    while (pool->count == 0)
        pthread_cond_wait(&pool->not_empty, &pool->lock); /* 잠그고 기다리면 잠금을 잠시 풀고 잔다 */

    *job = pool->jobs[pool->head];
    pool->head = (pool->head + 1) % POOL_QUEUE_SIZE;
    pool->count--;

    pthread_cond_signal(&pool->not_full);
    pthread_mutex_unlock(&pool->lock);
    return 0;
}
