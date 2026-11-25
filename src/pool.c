#include "pool.h"

#include <stdio.h>
#include <string.h>

/* 작업 스레드 본체: 큐에서 일감을 꺼내 처리하기를 반복한다. 일감이 없으면 잠든다.
 * 풀이 멈추는 중이고 큐가 비었으면 끝난다. */
static void *run_worker(void *arg)
{
    struct thread_pool *pool = arg;
    struct pool_job job;
    while (pool_take(pool, &job) == 0)
        pool->handler(&job, pool->arg);
    return NULL;
}

/* 큐와 잠금을 준비하고 작업 스레드 POOL_THREADS개를 띄운다. 일감마다 handler(job, arg)를 부른다.
 * 성공 0, 스레드를 못 만들면 이미 띄운 스레드를 정리하고 -1. */
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
            pool_stop(pool);
            return -1;
        }
        pool->thread_count++;
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

/* [작업 스레드] 큐 앞에서 일감 하나를 꺼내 job에 넣는다. 큐가 비었으면 일감이 올 때까지 잠든다.
 * 꺼냈으면 0. 풀이 멈추는 중이고 남은 일감도 없으면 -1(스레드가 끝날 차례). */
int pool_take(struct thread_pool *pool, struct pool_job *job)
{
    pthread_mutex_lock(&pool->lock);
    while (pool->count == 0 && !pool->stopping)
        pthread_cond_wait(&pool->not_empty, &pool->lock); /* 잠그고 기다리면 잠금을 잠시 풀고 잔다 */

    if (pool->count == 0) { /* 멈추는 중 + 남은 일감 없음 */
        pthread_mutex_unlock(&pool->lock);
        return -1;
    }
    *job = pool->jobs[pool->head];
    pool->head = (pool->head + 1) % POOL_QUEUE_SIZE;
    pool->count--;

    pthread_cond_signal(&pool->not_full);
    pthread_mutex_unlock(&pool->lock);
    return 0;
}

/* 큐에서 처리를 기다리는 연결이 있으면 1. 작업 스레드가 keep-alive 연결을 계속 붙잡을지 정할 때 본다. */
int pool_has_waiting(struct thread_pool *pool)
{
    pthread_mutex_lock(&pool->lock);
    int waiting = pool->count > 0;
    pthread_mutex_unlock(&pool->lock);
    return waiting;
}

/* [main] 풀을 멈춘다. 큐에 남은 일감과 처리 중인 연결은 끝까지 처리하게 두고,
 * 모든 작업 스레드가 끝날 때까지 기다린 뒤 잠금을 정리한다. */
void pool_stop(struct thread_pool *pool)
{
    pthread_mutex_lock(&pool->lock);
    pool->stopping = 1;
    pthread_cond_broadcast(&pool->not_empty); /* 잠든 작업 스레드를 전부 깨워 "끝낼 때"인지 보게 한다 */
    pthread_mutex_unlock(&pool->lock);

    for (int i = 0; i < pool->thread_count; i++)
        pthread_join(pool->threads[i], NULL); /* 그 스레드가 끝날 때까지 기다린다 */

    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->not_empty);
    pthread_cond_destroy(&pool->not_full);
}
