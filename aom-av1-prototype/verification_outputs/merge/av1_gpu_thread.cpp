#include "av1_gpu_thread.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

/* Note: If AV1_MEM_OVERRIDE is defined, applications can provide custom
 * memory allocation via av1_mem_malloc/av1_mem_free. This allows the decoder
 * to use memory from specific pools (e.g., DMA-able memory for GPU access).
 * For this stub implementation, we use system malloc since no GPU memory
 * pool is required. When GPU memory management is added, this should be
 * changed to use av1_mem_malloc for the thread struct and job queue. */
/* TODO: Add av1_mem_malloc.h include and replace calloc with av1_mem_malloc
 * when AV1_MEM_OVERRIDE is active for proper DMA-able memory allocation. */

#define NSEC_PER_USEC 1000ULL
#define STUB_PROCESSING_DELAY_US 1000

static void* gpu_thread_main(void *arg);

Av1GpuThread* av1_gpu_thread_create(int queue_depth, void *gpu_device) {
    if (queue_depth <= 0) {
        queue_depth = AV1_GPU_QUEUE_DEPTH;
    }
    
    Av1GpuThread *gt = static_cast<Av1GpuThread*>(calloc(1, sizeof(Av1GpuThread)));
    if (!gt) {
        return NULL;
    }
    
    /* Allocate queue entries - must match Av1GpuThread.jobs type (Av1GpuQueueEntry) */
    gt->jobs = static_cast<decltype(gt->jobs)>(calloc(queue_depth, sizeof(gt->jobs[0])));
    if (!gt->jobs) {
        free(gt);
        return NULL;
    }
    
    gt->capacity = queue_depth;
    gt->head = 0;
    gt->tail = 0;
    gt->count = 0;
    gt->exit_flag = 0;
    gt->gpu_device = gpu_device;
    
    if (pthread_mutex_init(&gt->mutex, NULL) != 0) {
        free(gt->jobs);
        free(gt);
        return NULL;
    }
    
    if (pthread_cond_init(&gt->job_available, NULL) != 0) {
        pthread_mutex_destroy(&gt->mutex);
        free(gt->jobs);
        free(gt);
        return NULL;
    }
    
    if (pthread_cond_init(&gt->job_complete, NULL) != 0) {
        pthread_cond_destroy(&gt->job_available);
        pthread_mutex_destroy(&gt->mutex);
        free(gt->jobs);
        free(gt);
        return NULL;
    }
    
    if (pthread_create(&gt->thread, NULL, gpu_thread_main, gt) != 0) {
        pthread_cond_destroy(&gt->job_complete);
        pthread_cond_destroy(&gt->job_available);
        pthread_mutex_destroy(&gt->mutex);
        free(gt->jobs);
        free(gt);
        return NULL;
    }
    
    return gt;
}

int av1_gpu_thread_enqueue(Av1GpuThread *gt, Av1GpuJob *job) {
    if (!gt || !job) {
        return -1;
    }
    
    job->status.store(AV1_GPU_JOB_PENDING);
    
    pthread_mutex_lock(&gt->mutex);
    
    if (gt->count >= gt->capacity) {
        pthread_mutex_unlock(&gt->mutex);
        return -1;
    }
    
    gt->jobs[gt->tail].job = job;
    gt->jobs[gt->tail].enqueued = 1;
    gt->tail = (gt->tail + 1) % gt->capacity;
    gt->count++;
    
    pthread_cond_signal(&gt->job_available);
    
    pthread_mutex_unlock(&gt->mutex);
    
    return 0;
}

int av1_gpu_thread_wait(Av1GpuThread *gt, Av1GpuJob *job, uint32_t timeout_us) {
    if (!gt || !job) {
        return -1;
    }
    
    pthread_mutex_lock(&gt->mutex);
    
    /* Wait until job is complete or times out */
    if (timeout_us == 0) {
        /* Infinite wait */
        while (job->status.load() != AV1_GPU_JOB_COMPLETE) {
            pthread_cond_wait(&gt->job_complete, &gt->mutex);
        }
    } else {
        /* Timed wait */
        struct timespec ts;
        if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
            pthread_mutex_unlock(&gt->mutex);
            return -1;
        }
        
        uint64_t nsec = (uint64_t)ts.tv_nsec + (uint64_t)timeout_us * NSEC_PER_USEC;
        ts.tv_sec += (int)(nsec / 1000000000ULL);
        ts.tv_nsec = (long)(nsec % 1000000000ULL);
        
        while (job->status.load() != AV1_GPU_JOB_COMPLETE) {
            int ret = pthread_cond_timedwait(&gt->job_complete, &gt->mutex, &ts);
            if (ret == ETIMEDOUT) {
                pthread_mutex_unlock(&gt->mutex);
                return -1;
            }
            if (ret != 0) {
                pthread_mutex_unlock(&gt->mutex);
                return -1;
            }
        }
    }
    
    int final_status = job->status.load();
    pthread_mutex_unlock(&gt->mutex);
    
    return (final_status == AV1_GPU_JOB_COMPLETE) ? 0 : -1;
}

int av1_gpu_thread_get_status(Av1GpuJob *job) {
    if (!job) {
        return -1;
    }
    return job->status.load();
}

int av1_gpu_thread_destroy(Av1GpuThread *gt) {
    if (!gt) {
        return -1;
    }
    
    pthread_mutex_lock(&gt->mutex);
    gt->exit_flag = 1;
    pthread_cond_broadcast(&gt->job_available);
    pthread_mutex_unlock(&gt->mutex);
    
    pthread_join(gt->thread, NULL);
    
    pthread_cond_destroy(&gt->job_complete);
    pthread_cond_destroy(&gt->job_available);
    pthread_mutex_destroy(&gt->mutex);
    
    free(gt->jobs);
    free(gt);
    
    return 0;
}

static void* gpu_thread_main(void *arg) {
    Av1GpuThread *gt = (Av1GpuThread*)arg;
    
    pthread_mutex_lock(&gt->mutex);
    
    while (!gt->exit_flag) {
        while (gt->count == 0 && !gt->exit_flag) {
            pthread_cond_wait(&gt->job_available, &gt->mutex);
        }
        
        if (gt->exit_flag) {
            break;
        }
        
        /* Dequeue job */
        auto *entry = &gt->jobs[gt->head];
        Av1GpuJob *job = entry->job;
        
        gt->head = (gt->head + 1) % gt->capacity;
        gt->count--;
        entry->enqueued = 0;
        
        job->status.store(AV1_GPU_JOB_PROCESSING);
        
        pthread_mutex_unlock(&gt->mutex);
        
        /* STUB: Simulate GPU processing */
        usleep(STUB_PROCESSING_DELAY_US);
        
        job->status.store(AV1_GPU_JOB_COMPLETE);
        
        pthread_mutex_lock(&gt->mutex);
        pthread_cond_broadcast(&gt->job_complete);
    }
    
    pthread_mutex_unlock(&gt->mutex);
    
    return NULL;
}
