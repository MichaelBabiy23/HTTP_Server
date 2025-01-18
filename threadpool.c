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
