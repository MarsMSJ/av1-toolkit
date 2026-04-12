#include "av1_job_queue.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#define NSEC_PER_USEC 1000ULL
#define USEC_PER_SEC  1000000ULL

int av1_frame_queue_init(Av1FrameQueue *q, Av1FrameEntry *storage, int capacity) {
    if (!q || !storage || capacity <= 0) {
        return -1;
    }

    memset(q, 0, sizeof(*q));
    q->entries = storage;
    q->capacity = capacity;
    q->head = 0;
    q->tail = 0;
    q->count = 0;

    if (pthread_mutex_init(&q->mutex, NULL) != 0) {
        return -1;
    }

    if (pthread_cond_init(&q->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&q->mutex);
        return -1;
    }

    if (pthread_cond_init(&q->not_full, NULL) != 0) {
        pthread_cond_destroy(&q->not_empty);
        pthread_mutex_destroy(&q->mutex);
        return -1;
    }

    return 0;
}

int av1_frame_queue_push(Av1FrameQueue *q, const Av1FrameEntry *entry) {
    if (!q || !entry) {
        return -1;
    }

    pthread_mutex_lock(&q->mutex);

    if (q->count >= q->capacity) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }

    memcpy(&q->entries[q->tail], entry, sizeof(*entry));
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

int av1_frame_queue_pop(Av1FrameQueue *q, Av1FrameEntry *out, uint32_t timeout_us) {
    if (!q || !out) {
        return -1;
    }

    pthread_mutex_lock(&q->mutex);

    if (q->count == 0) {
        if (timeout_us == 0) {
            pthread_mutex_unlock(&q->mutex);
            return -1;
        }

        struct timespec ts;
        if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
            pthread_mutex_unlock(&q->mutex);
            return -1;
        }

        /* Calculate absolute timeout with overflow check */
        uint64_t timeout_nsec = (uint64_t)timeout_us * NSEC_PER_USEC;
        uint64_t total_nsec = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
        uint64_t deadline = total_nsec + timeout_nsec;

        ts.tv_sec = (long)(deadline / 1000000000ULL);
        ts.tv_nsec = (long)(deadline % 1000000000ULL);

        int ret = pthread_cond_timedwait(&q->not_empty, &q->mutex, &ts);

        if (ret == ETIMEDOUT) {
            pthread_mutex_unlock(&q->mutex);
            return -1;
        }

        if (ret != 0) {
            pthread_mutex_unlock(&q->mutex);
            return -1;
        }

        if (q->count == 0) {
            pthread_mutex_unlock(&q->mutex);
            return -1;
        }
    }

    memcpy(out, &q->entries[q->head], sizeof(*out));
    q->head = (q->head + 1) % q->capacity;
    q->count--;

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

int av1_frame_queue_count(Av1FrameQueue *q) {
    if (!q) {
        return -1;
    }

    pthread_mutex_lock(&q->mutex);
    int cnt = q->count;
    pthread_mutex_unlock(&q->mutex);

    return cnt;
}

int av1_frame_queue_is_full(Av1FrameQueue *q) {
    if (!q) {
        return -1;
    }

    pthread_mutex_lock(&q->mutex);
    int full = (q->count >= q->capacity);
    pthread_mutex_unlock(&q->mutex);

    return full;
}

void av1_frame_queue_destroy(Av1FrameQueue *q) {
    if (!q) {
        return;
    }

    /* Wake all waiting threads so they can detect queue destruction */
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);

    /* Lock and unlock mutex to allow waiting threads to proceed.
     * They will wake, acquire the mutex, see the queue state, and return. */
    pthread_mutex_lock(&q->mutex);
    pthread_mutex_unlock(&q->mutex);

    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->mutex);

    memset(q, 0, sizeof(*q));
}
