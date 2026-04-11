#ifndef AV1_JOB_QUEUE_H
#define AV1_JOB_QUEUE_H

#include <stdint.h>
#include <pthread.h>

typedef struct Av1FrameEntry {
    uint32_t frame_id;
    int      dpb_slot;
    int      show_frame;
    int      show_existing_frame;
} Av1FrameEntry;

typedef struct Av1FrameQueue {
    Av1FrameEntry *entries;      // circular buffer [capacity]
    int            capacity;     // max entries (= queue_depth)
    int            head;         // next to dequeue
    int            tail;         // next to enqueue
    int            count;        // current entries
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;   // signaled when entry is enqueued
    pthread_cond_t  not_full;    // signaled when entry is dequeued
} Av1FrameQueue;

/**
 * Initialize a frame queue with caller-provided storage.
 * @param q        Queue to initialize
 * @param storage  Pre-allocated array of Av1FrameEntry[capacity]
 * @param capacity Number of entries in storage
 * @return 0 on success, -1 on error
 */
int av1_frame_queue_init(Av1FrameQueue *q, Av1FrameEntry *storage, int capacity);

/**
 * Push an entry to the queue (non-blocking).
 * @param q     Queue
 * @param entry Entry to enqueue
 * @return 0 on success, -1 if queue is full
 */
int av1_frame_queue_push(Av1FrameQueue *q, const Av1FrameEntry *entry);

/**
 * Pop an entry from the queue.
 * @param q          Queue
 * @param out        Output buffer for dequeued entry
 * @param timeout_us Timeout in microseconds. 0 = non-blocking poll.
 *                   If timeout > 0, blocks up to timeout_us microseconds.
 * @return 0 on success, -1 on timeout or error
 */
int av1_frame_queue_pop(Av1FrameQueue *q, Av1FrameEntry *out, uint32_t timeout_us);

/**
 * Get current number of entries in queue.
 * @param q Queue
 * @return Current count
 */
int av1_frame_queue_count(Av1FrameQueue *q);

/**
 * Check if queue is full.
 * @param q Queue
 * @return 1 if full, 0 otherwise
 */
int av1_frame_queue_is_full(Av1FrameQueue *q);

/**
 * Destroy a frame queue (cleanup mutex/condvar).
 * @param q Queue to destroy
 */
void av1_frame_queue_destroy(Av1FrameQueue *q);

#endif // AV1_JOB_QUEUE_H
