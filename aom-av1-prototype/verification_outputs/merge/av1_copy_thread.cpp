#include "av1_copy_thread.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>

#define NSEC_PER_USEC 1000ULL
#define USEC_PER_SEC  1000000ULL

/* Internal queue entry */
typedef struct Av1CopyQueueEntry {
    Av1CopyJob *job;
    int         enqueued;  /* 1 if entry contains a valid job */
} Av1CopyQueueEntry;

struct Av1CopyThread {
    Av1CopyQueueEntry *queue;      /* circular buffer */
    int                capacity;   /* max queue entries */
    int                head;       /* next to dequeue */
    int                tail;       /* next to enqueue */
    int                count;      /* current jobs in queue */
    
    pthread_mutex_t    mutex;
    pthread_cond_t     job_available;  /* signaled when job is enqueued */
    pthread_cond_t     job_complete;   /* signaled when job completes */
    
    pthread_t          thread;
    int                running;    /* 1 while thread should run */
    int                shutdown;   /* 1 when shutdown requested */
};

/* Forward declarations */
static void* copy_thread_main(void *arg);
static void copy_job_execute(Av1CopyJob *job);

Av1CopyThread* av1_copy_thread_create(int queue_depth) {
    if (queue_depth <= 0) {
        queue_depth = AV1_COPY_QUEUE_DEPTH;
    }
    
    Av1CopyThread *ct = static_cast<Av1CopyThread*>(calloc(1, sizeof(Av1CopyThread)));
    if (!ct) {
        return NULL;
    }
    
    ct->queue = static_cast<Av1CopyQueueEntry*>(calloc(queue_depth, sizeof(Av1CopyQueueEntry)));
    if (!ct->queue) {
        free(ct);
        return NULL;
    }
    
    ct->capacity = queue_depth;
    ct->head = 0;
    ct->tail = 0;
    ct->count = 0;
    ct->running = 1;
    ct->shutdown = 0;
    
    /* Initialize synchronization primitives */
    if (pthread_mutex_init(&ct->mutex, NULL) != 0) {
        free(ct->queue);
        free(ct);
        return NULL;
    }
    
    if (pthread_cond_init(&ct->job_available, NULL) != 0) {
        pthread_mutex_destroy(&ct->mutex);
        free(ct->queue);
        free(ct);
        return NULL;
    }
    
    if (pthread_cond_init(&ct->job_complete, NULL) != 0) {
        pthread_cond_destroy(&ct->job_available);
        pthread_mutex_destroy(&ct->mutex);
        free(ct->queue);
        free(ct);
        return NULL;
    }
    
    /* Start the copy thread */
    if (pthread_create(&ct->thread, NULL, copy_thread_main, ct) != 0) {
        pthread_cond_destroy(&ct->job_complete);
        pthread_cond_destroy(&ct->job_available);
        pthread_mutex_destroy(&ct->mutex);
        free(ct->queue);
        free(ct);
        return NULL;
    }
    
    return ct;
}

int av1_copy_thread_enqueue(Av1CopyThread *ct, Av1CopyJob *job) {
    if (!ct || !job) {
        return -1;
    }
    
    /* Initialize job status */
    job->status.store(AV1_COPY_PENDING);
    
    pthread_mutex_lock(&ct->mutex);
    
    /* Check if queue is full */
    if (ct->count >= ct->capacity) {
        pthread_mutex_unlock(&ct->mutex);
        return -1;
    }
    
    /* Enqueue the job */
    ct->queue[ct->tail].job = job;
    ct->queue[ct->tail].enqueued = 1;
    ct->tail = (ct->tail + 1) % ct->capacity;
    ct->count++;
    
    /* Signal the copy thread that a job is available */
    pthread_cond_signal(&ct->job_available);
    
    pthread_mutex_unlock(&ct->mutex);
    
    return 0;
}

int av1_copy_thread_wait(Av1CopyThread *ct, Av1CopyJob *job, uint32_t timeout_us) {
    if (!ct || !job) {
        return -1;
    }
    
    pthread_mutex_lock(&ct->mutex);
    
    /* Check current status without waiting */
    int current_status = job->status.load();
    if (current_status == AV1_COPY_COMPLETE) {
        pthread_mutex_unlock(&ct->mutex);
        return 0;
    }
    
    /* Wait for job to transition from PENDING or IN_PROGRESS to COMPLETE */
    if (current_status == AV1_COPY_PENDING || current_status == AV1_COPY_IN_PROGRESS) {
        if (timeout_us == 0) {
            /* Infinite wait */
            while (job->status.load() != AV1_COPY_COMPLETE) {
                pthread_cond_wait(&ct->job_complete, &ct->mutex);
            }
        } else {
            /* Timed wait */
            struct timespec ts;
            if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
                pthread_mutex_unlock(&ct->mutex);
                return -1;
            }
            
            uint64_t nsec = (uint64_t)ts.tv_nsec + (uint64_t)timeout_us * NSEC_PER_USEC;
            ts.tv_sec += (int)(nsec / 1000000000ULL);
            ts.tv_nsec = (long)(nsec % 1000000000ULL);
            
            while (job->status.load() != AV1_COPY_COMPLETE) {
                int ret = pthread_cond_timedwait(&ct->job_complete, &ct->mutex, &ts);
                if (ret == ETIMEDOUT) {
                    pthread_mutex_unlock(&ct->mutex);
                    return -1;
                }
                if (ret != 0) {
                    pthread_mutex_unlock(&ct->mutex);
                    return -1;
                }
            }
        }
    }
    
    /* Check final status */
    int final_status = job->status.load();
    pthread_mutex_unlock(&ct->mutex);
    
    return (final_status == AV1_COPY_COMPLETE) ? 0 : -1;
}

int av1_copy_thread_get_status(Av1CopyJob *job) {
    if (!job) {
        return -1;
    }
    return job->status.load();
}

int av1_copy_thread_destroy(Av1CopyThread *ct) {
    if (!ct) {
        return -1;
    }
    
    pthread_mutex_lock(&ct->mutex);
    ct->running = 0;  /* Signal thread to exit */
    ct->shutdown = 1;
    pthread_cond_broadcast(&ct->job_available);  /* Wake thread to exit */
    pthread_mutex_unlock(&ct->mutex);
    
    /* Wait for thread to terminate */
    pthread_join(ct->thread, NULL);
    
    /* Clean up synchronization primitives */
    pthread_cond_destroy(&ct->job_complete);
    pthread_cond_destroy(&ct->job_available);
    pthread_mutex_destroy(&ct->mutex);
    
    /* Free resources */
    free(ct->queue);
    free(ct);
    
    return 0;
}

/* Main copy thread function */
static void* copy_thread_main(void *arg) {
    Av1CopyThread *ct = (Av1CopyThread*)arg;
    
    pthread_mutex_lock(&ct->mutex);
    
    while (ct->running) {
        /* Wait for a job to become available */
        while (ct->count == 0 && ct->running) {
            pthread_cond_wait(&ct->job_available, &ct->mutex);
        }
        
        /* Check if we should exit */
        if (!ct->running) {
            break;
        }
        
        /* Dequeue the job */
        Av1CopyQueueEntry *entry = &ct->queue[ct->head];
        Av1CopyJob *job = entry->job;
        
        ct->head = (ct->head + 1) % ct->capacity;
        ct->count--;
        
        /* Mark entry as empty */
        entry->enqueued = 0;
        
        /* Set status to IN_PROGRESS and release lock during copy */
        job->status.store(AV1_COPY_IN_PROGRESS);
        
        pthread_mutex_unlock(&ct->mutex);
        
        /* Execute the copy job */
        copy_job_execute(job);
        
        /* Mark job as complete */
        job->status.store(AV1_COPY_COMPLETE);
        
        /* Signal any waiters that the job is done */
        pthread_mutex_lock(&ct->mutex);
        pthread_cond_broadcast(&ct->job_complete);
    }
    
    pthread_mutex_unlock(&ct->mutex);
    
    return NULL;
}

/* Execute a copy job - copies all planes row by row */
static void copy_job_execute(Av1CopyJob *job) {
    if (!job) {
        return;
    }
    
    /* Copy Y, U, V planes */
    for (int plane = 0; plane < 3; plane++) {
        const uint8_t *src = job->src_planes[plane];
        uint8_t *dst = job->dst_planes[plane];
        int src_stride = job->src_strides[plane];
        int dst_stride = job->dst_strides[plane];
        int width_bytes = job->plane_widths[plane];
        int height = job->plane_heights[plane];
        
        if (!src || !dst || width_bytes <= 0 || height <= 0) {
            continue;
        }
        
        /* Copy row by row using memcpy */
        for (int row = 0; row < height; row++) {
            const uint8_t *src_row = src + (row * src_stride);
            uint8_t *dst_row = dst + (row * dst_stride);
            memcpy(dst_row, src_row, width_bytes);
        }
    }
}
