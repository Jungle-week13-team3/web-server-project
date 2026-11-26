/* pool.c를 소켓 없이 확인하는 단위 테스트: 일감이 하나도 빠지거나 겹치지 않고, 여러 스레드가 나눠 처리하는지 본다. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "pool.h"

#define JOBS 2000 /* 큐(64칸)보다 훨씬 많이 넣어서 "꽉 차면 기다리기"도 지나가게 한다 */

static pthread_mutex_t seen_lock = PTHREAD_MUTEX_INITIALIZER;
static int seen[JOBS];      /* 일감 번호별로 몇 번 처리됐는지 */
static int done;            /* 처리된 일감 수 */
static pthread_t workers[POOL_THREADS];
static int worker_count;    /* 일감을 처리해 본 서로 다른 스레드 수 */

/* 일감 하나를 처리한 것으로 기록한다. connfd 자리에 일감 번호를 넣어 보냈다. */
static void record_job(const struct pool_job *job, void *arg)
{
    (void)arg;
    usleep(100); /* 처리에 시간이 조금 걸려야 여러 스레드가 나눠 맡는다 */
    pthread_mutex_lock(&seen_lock);
    seen[job->connfd]++;
    done++;
    int known = 0;
    for (int i = 0; i < worker_count; i++)
        known |= pthread_equal(workers[i], pthread_self());
    if (!known && worker_count < POOL_THREADS)
        workers[worker_count++] = pthread_self();
    pthread_mutex_unlock(&seen_lock);
}

int main(void)
{
    static struct thread_pool pool;
    assert(pool_start(&pool, record_job, NULL) == 0);

    for (int i = 0; i < JOBS; i++) {
        struct pool_job job = { .connfd = i, .port = 0 };
        pool_submit(&pool, &job);
    }

    /* 다 처리될 때까지 최대 10초 기다린다 */
    for (int waited = 0; waited < 1000; waited++) {
        pthread_mutex_lock(&seen_lock);
        int finished = (done == JOBS);
        pthread_mutex_unlock(&seen_lock);
        if (finished)
            break;
        usleep(10000);
    }

    pthread_mutex_lock(&seen_lock);
    assert(done == JOBS);
    for (int i = 0; i < JOBS; i++)
        assert(seen[i] == 1); /* 빠짐도 겹침도 없다 */
    assert(worker_count > 1); /* 스레드 하나가 다 한 게 아니다 */
    printf("all pool unit tests passed (%d jobs, %d threads used)\n", JOBS, worker_count);
    pthread_mutex_unlock(&seen_lock);
    return 0;
}
