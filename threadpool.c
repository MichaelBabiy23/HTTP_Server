#include "threadpool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

threadpool* create_threadpool(int num_threads_in_pool, int max_queue_size) {
    if (num_threads_in_pool <= 0 || num_threads_in_pool > MAXT_IN_POOL ||
        max_queue_size <= 0 || max_queue_size > MAXW_IN_QUEUE) {
        fprintf(stderr, "Invalid parameters for threadpool creation.\n");
        return NULL;
        }

    threadpool *pool = (threadpool *)malloc(sizeof(threadpool));
    if (!pool) {
        perror("Failed to allocate memory for threadpool");
        return NULL;
    }

    // Initialize threadpool structure
    pool->num_threads = num_threads_in_pool;
    pool->max_qsize = max_queue_size;
    pool->qsize = 0;
    pool->qhead = NULL;
    pool->qtail = NULL;
    pool->shutdown = 0;
    pool->dont_accept = 0;

    // Initialize mutex and condition variables
    if (pthread_mutex_init(&(pool->qlock), NULL) != 0 ||
        pthread_cond_init(&(pool->q_not_empty), NULL) != 0 ||
        pthread_cond_init(&(pool->q_empty), NULL) != 0 ||
        pthread_cond_init(&(pool->q_not_full), NULL) != 0) {
        perror("Failed to initialize mutex or condition variables");
        free(pool);
        return NULL;
        }

    // Allocate memory for threads
    pool->threads = (pthread_t *)malloc(sizeof(pthread_t) * num_threads_in_pool);
    if (!pool->threads) {
        perror("Failed to allocate memory for threads");
        pthread_mutex_destroy(&(pool->qlock));
        pthread_cond_destroy(&(pool->q_not_empty));
        pthread_cond_destroy(&(pool->q_empty));
        pthread_cond_destroy(&(pool->q_not_full));
        free(pool);
        return NULL;
    }

    // Create threads
    for (int i = 0; i < num_threads_in_pool; i++) {
        if (pthread_create(&(pool->threads[i]), NULL, do_work, (void *)pool) != 0) {
            perror("Failed to create thread");
            destroy_threadpool(pool);
            return NULL;
        }
    }

    return pool;
}

void dispatch(threadpool* from_me, int (*dispatch_to_here)(void *), void *arg) {
    if (!from_me || !dispatch_to_here) {
        fprintf(stderr, "Invalid parameters for dispatch.\n");
        return;
    }

    pthread_mutex_lock(&(from_me->qlock));

    // If the pool is shutting down or not accepting jobs
    if (from_me->dont_accept) {
        pthread_mutex_unlock(&(from_me->qlock));
        return;
    }

    // Wait if the queue is full
    while (from_me->qsize >= from_me->max_qsize) {
        pthread_cond_wait(&(from_me->q_not_full), &(from_me->qlock));
    }

    // Create and initialize a new work item
    work_t *work = (work_t *)malloc(sizeof(work_t));
    if (!work) {
        perror("Failed to allocate memory for work item");
        pthread_mutex_unlock(&(from_me->qlock));
        return;
    }
    work->routine = dispatch_to_here;
    work->arg = arg;
    work->next = NULL;

    // Add the work item to the queue
    if (from_me->qtail) {
        from_me->qtail->next = work;
    } else {
        from_me->qhead = work;
    }
    from_me->qtail = work;
    from_me->qsize++;

    // Signal that the queue is not empty
    pthread_cond_signal(&(from_me->q_not_empty));

    pthread_mutex_unlock(&(from_me->qlock));
}

void* do_work(void* p) {
    threadpool *pool = (threadpool *)p;

    while (1) {
        pthread_mutex_lock(&(pool->qlock));

        // Wait until there is work or the pool is shutting down
        while (pool->qsize == 0 && !pool->shutdown) {
            pthread_cond_wait(&(pool->q_not_empty), &(pool->qlock));
        }

        // Exit if the pool is shutting down
        if (pool->shutdown) {
            pthread_mutex_unlock(&(pool->qlock));
            pthread_exit(NULL);
        }

        // Get the next work item
        work_t *work = pool->qhead;
        if (work) {
            pool->qhead = work->next;
            if (!pool->qhead) {
                pool->qtail = NULL;
            }
            pool->qsize--;

            // Signal that there is space in the queue
            if (pool->qsize < pool->max_qsize) {
                pthread_cond_signal(&(pool->q_not_full));
            }
        }

        pthread_mutex_unlock(&(pool->qlock));

        // Execute the work routine
        if (work) {
            work->routine(work->arg);
            free(work);
        }
    }

    return NULL;
}