

## Code Review: av1_job_queue.c

After analyzing the code, I found several bugs:

### Bug 1: Missing pthread_cond_broadcast in destroy (Potential Deadlock)
**File:** av1_job_queue.c  
**Approximate line:** ~150 (av1_frame_queue_destroy function)

**Problem:** When `av1_frame_queue_destroy` is called while other threads are waiting on `pthread_cond_wait` or `pthread_cond_timedwait`, those threads will never wake up because no signal is sent. This causes a deadlock if the caller tries to join those threads afterward.

**Fix:** Add `pthread_cond_broadcast(&q->not_empty)` and `pthread_cond_broadcast(&q->not_full)` before destroying the condition variables.

---

### Bug 2: No Handling of Spurious Wakeups
**File:** av1_job_queue.c  
**Approximate line:** ~90-105 (av1_frame_queue_pop function)

**Problem:** Per POSIX, `pthread_cond_timedwait` can return without the condition being signaled (spurious wakeup). The code doesn't re-check the condition in a loop after waking - it only checks once before the wait. If a spurious wakeup occurs, the code proceeds with `count == 0` and could read garbage or block indefinitely.

**Fix:** Wrap the wait in a `while` loop that re-checks `q->count == 0`.

---

### Bug 3: Using CLOCK_REALTIME for Timeout (Robustness Issue)
**File:** av1_job_queue.c  
**Approximate line:** ~80 (av1_frame_queue_pop function)

**Problem:** `CLOCK_REALTIME` can jump forward or backward due to system time changes (NTP, manual adjustments), causing the timeout to be incorrect. `CLOCK_MONOTONIC` should be used for timeouts.

**Fix:** Use `CLOCK_MONOTONIC` instead of `CLOCK_REALTIME`.

---

### Corrected Files

### av1_job_queue.c

```c
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

    /* Bug fix: Use while loop to handle spurious wakeups */
    while (q->count == 0) {
        if (timeout_us == 0) {
            pthread_mutex_unlock(&q->mutex);
            return -1;
        }

        struct timespec ts;
        /* Bug fix: Use CLOCK_MONOTONIC instead of CLOCK_REALTIME */
        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
            pthread_mutex_unlock(&q->mutex);
            return -1;
        }

        uint64_t nsec = (uint64_t)ts.tv_nsec + (uint64_t)timeout_us * NSEC_PER_USEC;
        ts.tv_sec += (int)(nsec / (1000000000ULL));
        ts.tv_nsec = (long)(nsec % (1000000000ULL));

        int ret = pthread_cond_timedwait(&q->not_empty, &q->mutex, &ts);

        if (ret == ETIMEDOUT) {
            pthread_mutex_unlock(&q->mutex);
            return -1;
        }

        if (ret != 0) {
            pthread_mutex_unlock(&q->mutex);
            return -1;
        }

        /* Loop back to re-check count - handles spurious wakeups */
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

    /* Bug fix: Signal all waiting threads before destroying to avoid deadlock */
    pthread_cond_broadcast(&q->not_full);
    pthread_cond_broadcast(&q->not_empty);

    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->mutex);

    memset(q, 0, sizeof(*q));
}
```

### av1_job_queue.h

No bugs found in this file. It is clean and correct.