#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include "threadpool.h"

/*
 * do_work() is the thread entry point.
 * Threads loop here, waiting for work in the pool's queue.
 * Once work arrives, a thread removes it from the queue and processes it.
 */

void* do_work(void* p) {
    threadpool* pool = (threadpool*)p;

    while (1) {
        /* Lock the queue */
        pthread_mutex_lock(&(pool->qlock));

        /* If no work and not shutting down, wait */
        while ((pool->qsize == 0) && (!pool->shutdown)) {
            pthread_cond_wait(&(pool->q_not_empty), &(pool->qlock));
        }

        /* If we're shutting down, exit this thread */
        if (pool->shutdown) {
            pthread_mutex_unlock(&(pool->qlock));
            pthread_exit(NULL);
        }

        /* Now we have at least one work item in the queue */
        work_t* work = pool->qhead;
        if (work == NULL) {
            /* Spurious wakeup or something else? */
            pthread_mutex_unlock(&(pool->qlock));
            continue;
        }

        /* Remove the work from the queue */
        pool->qhead = work->next;
        pool->qsize--;

        /* If the queue is now empty, adjust qtail as well */
        if (pool->qsize == 0) {
            pool->qtail = NULL;
            /* If we are not accepting any more and the queue just became empty, notify destroy_threadpool */
            if (pool->dont_accept) {
                pthread_cond_broadcast(&(pool->q_empty));
            }
        }

        /* If queue was full before, signal that it's not full now */
        if (pool->qsize == (pool->max_qsize - 1)) {
            pthread_cond_broadcast(&(pool->q_not_full));
        }

        /* Unlock the queue so other threads can access it */
        pthread_mutex_unlock(&(pool->qlock));

        /* Perform the actual work routine */
        (*(work->routine))(work->arg);

        /* Free the work structure */
        free(work);
    }

    return NULL;
}

/*
 * create_threadpool() - creates a fixed-size thread pool of num_threads_in_pool threads.
 */
threadpool* create_threadpool(int num_threads_in_pool, int max_queue_size) {
    if (num_threads_in_pool <= 0 || num_threads_in_pool > MAXT_IN_POOL) {
        fprintf(stderr, "Error: Invalid number of threads.\n");
        return NULL;
    }
    if (max_queue_size <= 0 || max_queue_size > MAXW_IN_QUEUE) {
        fprintf(stderr, "Error: Invalid queue size.\n");
        return NULL;
    }

    threadpool* pool = (threadpool*)malloc(sizeof(threadpool));
    if (!pool) {
        perror("malloc");
        return NULL;
    }

    pool->num_threads = num_threads_in_pool;
    pool->qsize = 0;
    pool->max_qsize = max_queue_size;
    pool->threads = (pthread_t*)malloc(sizeof(pthread_t) * num_threads_in_pool);
    if (!pool->threads) {
        perror("malloc");
        free(pool);
        return NULL;
    }

    pool->qhead = NULL;
    pool->qtail = NULL;
    pool->shutdown = 0;
    pool->dont_accept = 0;

    pthread_mutex_init(&(pool->qlock), NULL);
    pthread_cond_init(&(pool->q_not_empty), NULL);
    pthread_cond_init(&(pool->q_empty), NULL);
    pthread_cond_init(&(pool->q_not_full), NULL);

    /* Create threads */
    for (int i = 0; i < num_threads_in_pool; i++) {
        if (pthread_create(&(pool->threads[i]), NULL, do_work, pool) != 0) {
            perror("pthread_create");
            /* If we fail here, we should set up a partial destruction,
               but for simplicity, we just clean up and return NULL */
            destroy_threadpool(pool);
            return NULL;
        }
    }

    return pool;
}

/*
 * dispatch() - add a work function to the queue of work that the threadpool must process.
 */
void dispatch(threadpool* from_me, dispatch_fn dispatch_to_here, void *arg) {
    if (!from_me || !dispatch_to_here) return;

    /* Create a new work_t struct */
    work_t* work = (work_t*)malloc(sizeof(work_t));
    if (!work) {
        /* If we cannot allocate, there's a bigger problem,
           but let's just return (or could send 500 to client). */
        perror("malloc");
        return;
    }
    work->routine = dispatch_to_here;
    work->arg = arg;
    work->next = NULL;

    /* Lock the queue for modifications */
    pthread_mutex_lock(&(from_me->qlock));

    /* If destroy_threadpool() has begun, we do not accept new work */
    if (from_me->dont_accept) {
        /* free the work and return */
        free(work);
        pthread_mutex_unlock(&(from_me->qlock));
        return;
    }

    /* If the queue is full, wait until it's not full */
    while ((from_me->qsize == from_me->max_qsize) && (!from_me->shutdown) && (!from_me->dont_accept)) {
        pthread_cond_wait(&(from_me->q_not_full), &(from_me->qlock));
    }

    if (from_me->shutdown || from_me->dont_accept) {
        /* No longer accepting jobs */
        free(work);
        pthread_mutex_unlock(&(from_me->qlock));
        return;
    }

    /* Add the new work to the tail of the queue */
    if (from_me->qsize == 0) {
        from_me->qhead = work;
        from_me->qtail = work;
        /* If queue was empty, now we must signal that it's not empty */
        pthread_cond_broadcast(&(from_me->q_not_empty));
    } else {
        from_me->qtail->next = work;
        from_me->qtail = work;
    }
    from_me->qsize++;

    pthread_mutex_unlock(&(from_me->qlock));
}

/*
 * destroy_threadpool() - gracefully shutdown the threadpool,
 * waiting for running jobs to finish, then free all resources.
 */
void destroy_threadpool(threadpool* destroyme) {
    if (!destroyme) return;

    pthread_mutex_lock(&(destroyme->qlock));

    /* Stop accepting new work */
    destroyme->dont_accept = 1;

    /* Wait for the queue to become empty, if necessary */
    while (destroyme->qsize != 0) {
        pthread_cond_wait(&(destroyme->q_empty), &(destroyme->qlock));
    }

    /* Now tell all threads to shutdown */
    destroyme->shutdown = 1;

    /* Wake up any threads that are waiting on empty/other conditions */
    pthread_cond_broadcast(&(destroyme->q_not_empty));
    pthread_cond_broadcast(&(destroyme->q_not_full));

    pthread_mutex_unlock(&(destroyme->qlock));

    /* Join all threads */
    for (int i = 0; i < destroyme->num_threads; i++) {
        pthread_join(destroyme->threads[i], NULL);
    }

    /* Cleanup */
    free(destroyme->threads);

    /* Delete any remaining work in the queue (should be empty by now) */
    work_t* cur = destroyme->qhead;
    while (cur) {
        work_t* tmp = cur;
        cur = cur->next;
        free(tmp);
    }

    pthread_mutex_destroy(&(destroyme->qlock));
    pthread_cond_destroy(&(destroyme->q_not_empty));
    pthread_cond_destroy(&(destroyme->q_empty));
    pthread_cond_destroy(&(destroyme->q_not_full));

    free(destroyme);
}

