# Implementation Prompts for MiniMax-M2.5

These prompts are designed to be fed sequentially to MiniMax-M2.5. Each prompt produces a verifiable artifact. The outputs of earlier prompts become context/inputs for later ones.

**Workflow**: Run prompt → verify output → correct if needed → feed corrected output into next prompt.

---

## Prompt 1: Pool Allocator

```
You are implementing a fixed-memory pool allocator for an AV1 video decoder.
This allocator operates on a single contiguous memory block provided by the caller.
It must never call malloc/free/calloc/realloc.

Implement the following in C (C11, no C++ features):

File: av1_pool_allocator.h / av1_pool_allocator.c

1. A bump allocator for init-time allocations:
   - av1_pool_init(Av1PoolAllocator *alloc, void *base, size_t size)
   - void *av1_pool_bump_alloc(Av1PoolAllocator *alloc, size_t size, size_t alignment)
     Returns NULL if out of space. Advances internal offset.
     All pointers must be aligned to the requested alignment.

2. A fixed-slot sub-allocator for frame buffers:
   - av1_pool_init_frame_slots(Av1PoolAllocator *alloc, int num_slots, size_t slot_size)
     Carves num_slots × slot_size from the bump allocator.
   - int av1_pool_acquire_frame_slot(Av1PoolAllocator *alloc)
     Returns slot index (0..num_slots-1) or -1 if all occupied.
     Uses an atomic bitmask for thread safety.
   - void av1_pool_release_frame_slot(Av1PoolAllocator *alloc, int slot_index)
     Marks slot as free via atomic bitmask.
   - void *av1_pool_get_frame_slot_ptr(Av1PoolAllocator *alloc, int slot_index)
     Returns pointer to slot's memory.

3. A debug/query function:
   - size_t av1_pool_bytes_used(const Av1PoolAllocator *alloc)
   - size_t av1_pool_bytes_remaining(const Av1PoolAllocator *alloc)

Requirements:
- Thread-safe slot acquire/release using C11 atomics (_Atomic, atomic_fetch_or, etc.)
- Bump allocator is NOT thread-safe (only called during single-threaded init)
- Max 32 frame slots (use uint32_t bitmask)
- Include static_assert that num_slots <= 32
- No external dependencies except <stdint.h>, <stddef.h>, <stdatomic.h>, <string.h>
- Write a test main() that: allocates a 64MB block, inits the pool, bumps several
  allocations with different alignments, creates 17 frame slots of 1MB each,
  acquires all 17, verifies the 18th fails, releases slot 5, re-acquires
  successfully, and prints bytes used/remaining.
```

### Verification checklist for Prompt 1:
- [ ] Bump allocator respects alignment (test with 1, 4, 16, 64 byte alignments)
- [ ] Frame slot acquire returns -1 when full
- [ ] Frame slot release + re-acquire works
- [ ] No calls to malloc/free anywhere
- [ ] Compiles with: `gcc -std=c11 -Wall -Wextra -pthread -o test av1_pool_allocator.c`
- [ ] Test main() runs clean under valgrind (no leaks — because no mallocs)

---

## Prompt 2: Lock-Free Ring Buffer (Job Queue)

```
Implement a fixed-size, single-producer single-consumer (SPSC) lock-free ring buffer in C11.
This will be used as the job queue between the decoder API thread and worker threads.

File: av1_job_queue.h / av1_job_queue.c

The ring buffer stores opaque job entries of a fixed size.

API:
- av1_queue_init(Av1Queue *q, void *storage, int capacity, size_t entry_size)
  storage is pre-allocated memory (from the pool allocator).
  capacity must be a power of 2.

- int av1_queue_push(Av1Queue *q, const void *entry)
  Returns 1 on success, 0 if full.
  Copies entry_size bytes from entry into the ring.
  Uses atomic store with release semantics on write_idx.

- int av1_queue_pop(Av1Queue *q, void *out_entry)
  Returns 1 on success, 0 if empty.
  Copies entry_size bytes into out_entry.
  Uses atomic load with acquire semantics on write_idx.
  Uses atomic store with release semantics on read_idx.

- int av1_queue_count(const Av1Queue *q)
  Returns number of entries currently in the queue.

- int av1_queue_is_full(const Av1Queue *q)
- int av1_queue_is_empty(const Av1Queue *q)

For the multi-producer case (multiple workers posting to ready queue), implement:

- av1_mpsc_queue_init(...)  — multiple producer, single consumer
  Uses a spinlock (atomic_flag) for producers; consumer is lock-free.
- int av1_mpsc_queue_push(Av1MpscQueue *q, const void *entry)
- int av1_mpsc_queue_pop(Av1MpscQueue *q, void *out_entry)

Requirements:
- C11 atomics only. No pthread mutexes for the SPSC queue.
- Memory ordering: use memory_order_acquire / memory_order_release appropriately.
- The MPSC spinlock must use atomic_flag_test_and_set / atomic_flag_clear.
- Write a test that spawns 2 threads: producer pushes 1M entries, consumer pops
  1M entries. Verify all entries received in order (SPSC) and all received (MPSC).
```

### Verification checklist for Prompt 2:
- [ ] SPSC: producer/consumer stress test with 1M entries passes
- [ ] MPSC: multiple producers, single consumer, all entries received
- [ ] No data races under ThreadSanitizer: `gcc -fsanitize=thread`
- [ ] Queue full/empty edge cases handled
- [ ] Power-of-2 capacity enforced (assert or error)

---

## Prompt 3: av1_query_memory Implementation

```
Implement the av1_query_memory function for an AV1 decoder.

Given stream characteristics (width, height, bit_depth, chroma subsampling,
monochrome flag) and pipeline configuration (queue_depth, num_worker_threads),
compute the total memory needed.

File: av1_memory_calc.h / av1_memory_calc.c

Use these formulas:

1. Frame buffer size:
   aligned_width  = ALIGN(width, 64)   // AV1 superblock alignment
   aligned_height = ALIGN(height, 64)
   bytes_per_sample = (bit_depth > 8) ? 2 : 1
   luma_size = aligned_width * aligned_height * bytes_per_sample
   chroma_size = (monochrome) ? 0 :
       (aligned_width >> subsampling_x) * (aligned_height >> subsampling_y) * bytes_per_sample
   frame_size = luma_size + 2 * chroma_size
   // Add border pixels (AOM uses 128 pixel border for motion compensation)
   border = 128
   bordered_width  = aligned_width  + 2 * border
   bordered_height = aligned_height + 2 * border
   // Recompute with borders for actual allocation

2. Per-frame overhead:
   mv_buffer  = (aligned_width/8) * (aligned_height/8) * sizeof(MV_REF)  // ~12 bytes each
   seg_map    = (aligned_width/4) * (aligned_height/4) * 1
   grain_buf  = frame_size  // film grain needs a separate output buffer

3. DPB:
   dpb_count = 8 + queue_depth + 1  // REF_FRAMES + pipeline + safety
   dpb_total = dpb_count * (frame_size_with_borders + mv_buffer + seg_map + grain_buf)

4. Worker scratch (per worker):
   mc_buf         = 2 * (128+8)*(128+8) * bytes_per_sample  // motion comp temp
   tmp_conv_dst   = 128 * 128 * sizeof(int32_t)
   obmc_bufs      = 2 * 128 * 128 * bytes_per_sample
   seg_mask       = 128 * 128 * 2
   worker_scratch = mc_buf + tmp_conv_dst + obmc_bufs + seg_mask + 4096 // padding
   total_scratch  = num_workers * worker_scratch

5. Entropy context:
   frame_context_size = 75000  // approximate sizeof(FRAME_CONTEXT) ~73KB
   entropy_total = queue_depth * frame_context_size

6. Decoder context + mode info:
   mi_size = (aligned_width/4) * (aligned_height/4) * 48  // approximate MB_MODE_INFO
   context_total = 65536 + mi_size  // decoder struct + tile data + tables

7. Queues:
   queue_total = 3 * queue_depth * 256 + 4096  // 3 queues + sync primitives

8. Alignment padding:
   padding = total * 0.05  // 5% headroom

Fill out an Av1MemoryRequirements struct with total and per-category breakdowns.

Write a test that queries memory for:
  - 1080p, 8-bit, 4:2:0, queue_depth=4, workers=4
  - 4K, 10-bit, 4:2:0, queue_depth=8, workers=8
  - 8K, 10-bit, 4:4:4, queue_depth=16, workers=16
Print the total and breakdown for each.
```

### Verification checklist for Prompt 3:
- [ ] 1080p 8-bit result is in the range ~150–300 MB
- [ ] 4K 10-bit result is in the range ~1–2 GB
- [ ] 8K 10-bit 4:4:4 result is in the range ~8–16 GB (expect large)
- [ ] Alignment macro works correctly
- [ ] No integer overflow for large resolutions (use size_t throughout)

---

## Prompt 4: Copy Thread Implementation

```
Implement the copy thread for the AV1 decoder.

The copy thread runs in a loop waiting for copy jobs. When the application calls
av1_set_output(), a copy job is enqueued. The copy thread performs the plane-by-plane
memcpy and signals completion so av1_receive_output() can return.

File: av1_copy_thread.h / av1_copy_thread.c

Structures:
typedef struct Av1CopyJob {
    uint32_t frame_id;
    int      dpb_slot;           // source DPB slot index
    void    *src_planes[3];      // Y, U, V source pointers
    int      src_strides[3];     // source strides
    void    *dst_planes[3];      // Y, U, V destination pointers
    int      dst_strides[3];     // destination strides
    int      plane_widths[3];    // bytes to copy per row
    int      plane_heights[3];   // number of rows per plane
    _Atomic int status;          // 0=pending, 1=in_progress, 2=complete
} Av1CopyJob;

typedef struct Av1CopyThread {
    pthread_t       thread;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    pthread_cond_t  done_cond;   // signaled when a copy completes
    Av1CopyJob     *jobs;        // circular buffer [queue_depth]
    int             queue_depth;
    int             head, tail;
    int             exit_flag;
    int             priority;    // OS thread priority to set
} Av1CopyThread;

Functions:
- av1_copy_thread_init(Av1CopyThread *ct, void *job_storage, int queue_depth, int priority)
  Creates and starts the thread with the given priority.
- av1_copy_thread_enqueue(Av1CopyThread *ct, const Av1CopyJob *job)
  Enqueues a copy job. Returns 0 on success.
- int av1_copy_thread_wait(Av1CopyThread *ct, uint32_t frame_id, uint32_t timeout_us)
  Blocks until the job with matching frame_id has status==complete.
  Returns 0 on success, -1 on timeout.
- av1_copy_thread_destroy(Av1CopyThread *ct)
  Sets exit_flag, signals cond, joins thread.

The copy loop:
  1. Lock mutex
  2. While queue empty and !exit_flag: wait on cond
  3. Dequeue job, set status=in_progress, unlock mutex
  4. memcpy each plane row by row
  5. Set status=complete (atomic store)
  6. Broadcast done_cond
  7. Repeat

Write a test that:
  - Creates a copy thread with queue_depth=4
  - Allocates two 1920×1080 YUV420 buffers (src filled with pattern, dst zeroed)
  - Enqueues a copy job
  - Waits for completion
  - Verifies dst matches src byte-for-byte
  - Enqueues 4 jobs rapidly, waits for each
  - Destroys the thread cleanly
```

### Verification checklist for Prompt 4:
- [ ] Copy produces byte-identical output
- [ ] Thread exits cleanly (no hangs on destroy)
- [ ] Multiple rapid enqueues work without corruption
- [ ] ThreadSanitizer clean
- [ ] Timeout path works (enqueue nothing, wait with 100ms timeout → returns -1)

---

## Prompt 5: av1_decode — OBU Parse / Enqueue Split

```
This prompt modifies AOM reference decoder code. You are given the existing function
aom_decode_frame_from_obus() from av1/decoder/obu.c.

Your task: split this function into two parts:

Part A: av1_parse_obu_headers_and_setup() — runs on the caller thread
  - Parses OBU headers (temporal delimiter, sequence header, frame header)
  - Calls av1_decode_frame_headers_and_setup() to set up frame params
  - Handles show_existing_frame (returns immediately with frame ready)
  - Locates tile group data pointers and sizes
  - Does NOT call av1_decode_tg_tiles_and_wrapup()
  - Returns a job descriptor containing all info needed for async decode

Part B: av1_async_decode_tiles(DecodeJob *job) — runs on a worker thread
  - Calls av1_decode_tg_tiles_and_wrapup() using the job descriptor
  - When complete, enqueues the frame_id to the ready queue

Define the DecodeJob structure that bridges Part A and Part B:
typedef struct Av1DecodeJob {
    uint32_t       frame_id;
    int            dpb_slot;
    int            show_frame;
    int            show_existing_frame;
    // Tile group info
    const uint8_t *tile_data;
    size_t         tile_data_size;
    int            start_tile;
    int            end_tile;
    // Frame-level pointers (already set up by Part A)
    AV1Decoder    *decoder;      // internal decoder state
    // Ready queue to post result to
    Av1MpscQueue  *ready_queue;
} Av1DecodeJob;

Write PSEUDOCODE (not compilable code) showing:
1. The exact lines from aom_decode_frame_from_obus() that go in Part A vs Part B
2. How the DecodeJob is populated between them
3. How show_existing_frame is handled as a fast path

Reference the AOM source code structure. Use line references from obu.c where possible.
This is a design document, not final code. The implementer will use this to make the
actual modifications.
```

### Verification checklist for Prompt 5:
- [ ] Part A contains ONLY parsing and setup — no entropy, no recon, no filtering
- [ ] Part B contains the full tile decode + filter + wrapup pipeline
- [ ] show_existing_frame handled entirely in Part A (no worker dispatch needed)
- [ ] DecodeJob contains all data Part B needs without re-parsing the bitstream
- [ ] Multi-tile-group OBUs are accounted for (AV1 allows multiple tile groups per frame)

---

## Prompt 6: av1_create_decoder — Full Initialization

```
Write pseudocode/detailed design for av1_create_decoder().

This function must:
1. Validate config (memory_size >= what query_memory said, queue_depth > 0, etc.)
2. Initialize the pool allocator on config->memory_base
3. Bump-allocate the Av1Decoder struct (aligned 64)
4. Initialize the frame slot allocator (dpb_count slots of frame_size)
5. Bump-allocate per-worker scratch buffers (num_worker_threads sets)
6. Bump-allocate entropy contexts (queue_depth × FRAME_CONTEXT)
7. Bump-allocate job queues (decode queue, ready queue, copy queue)
8. Initialize the internal AOM decoder state:
   - Zero and configure AV1Decoder fields
   - Initialize AV1_COMMON (reference frame map, buffer pool pointing to slot allocator)
   - Initialize FRAME_CONTEXT with default CDFs
9. Create worker threads with requested affinity and priority
10. Create copy thread with requested priority
11. Optionally create GPU thread if config->use_gpu
12. Set decoder state to CREATED
13. Return AV1_OK and set *out_decoder

For thread creation, use pthreads:
- pthread_create for each worker
- pthread_attr_setschedparam for priority
- Use platform-specific affinity (pthread_setaffinity_np on Linux,
  thread_policy_set on macOS — abstract behind a helper)

Show the initialization order and what happens if any step fails (cleanup).

This is pseudocode/design. Show the memory layout after init completes:
[Av1Decoder struct | DPB slots 0..N | Worker scratch 0..M | Entropy ctxs | Queues | ...]
```

### Verification checklist for Prompt 6:
- [ ] Memory layout is sequential with proper alignment between sections
- [ ] Failure at any step cleans up (joins threads, etc.) before returning error
- [ ] Pool allocator bytes_used after init < memory_size
- [ ] Thread priorities are set correctly per platform
- [ ] AOM internal state (AV1_COMMON, BufferPool) is properly initialized
- [ ] No calls to aom_malloc anywhere — all from pool

---

## Prompt 7: av1_sync, av1_flush, av1_destroy — Lifecycle Completion

```
Write the implementation design for the remaining API functions.

av1_sync(decoder, timeout_us, out_result):
  1. Check ready queue (non-blocking pop)
  2. If frame available: fill out_result with frame_id, frame_ready=1, return AV1_OK
  3. If timeout_us == 0: return AV1_NEED_MORE_DATA
  4. If timeout_us > 0: wait on ready_queue condvar with timeout
  5. If decoder is in FLUSHING state and ready_queue empty and work_queue empty:
     return AV1_END_OF_STREAM
  6. Handle show_existing_frame: these appear on ready_queue immediately after
     av1_decode() enqueues them.

av1_flush(decoder):
  1. Set decoder state to FLUSHING
  2. Signal all worker threads to drain their current jobs
  3. Do NOT accept new av1_decode() calls (return error if called)
  4. Return AV1_OK

av1_destroy_decoder(decoder):
  1. If not already flushed, flush first
  2. Set exit flags on all threads (workers, copy, gpu)
  3. Signal all condition variables
  4. Join all threads (pthread_join)
  5. Zero out the decoder struct (security: wipe state)
  6. Return AV1_OK
  7. After this, caller may free the memory block

Error handling table:
  - av1_decode when state != DECODING/CREATED → AV1_ERROR_INVALID_PARAM
  - av1_sync when state == UNINITIALIZED → AV1_ERROR_INVALID_PARAM
  - av1_set_output with invalid frame_id → AV1_ERROR_INVALID_PARAM
  - av1_receive_output when no set_output was called → AV1_ERROR_INVALID_PARAM
```

### Verification checklist for Prompt 7:
- [ ] av1_sync with timeout=0 never blocks
- [ ] av1_sync with timeout correctly uses timed wait
- [ ] Flush → repeated sync eventually returns END_OF_STREAM
- [ ] Destroy joins all threads without deadlock
- [ ] State transitions match the state machine in 02-state-machine.md

---

## Prompt 8: GPU Thread Design — Symbol Buffer Interface

```
Design the GPU thread and its interface with the CPU entropy decode workers.

Context: In GPU-assisted mode, CPU workers perform ONLY entropy/CABAC decoding.
The result (decoded symbols: coefficients, mode info, motion vectors) is written
into a "symbol buffer." The GPU thread picks up this buffer, uploads it to GPU
memory, builds command buffers for reconstruction + filtering, submits to the GPU,
and waits for completion.

Design the following:

1. Av1SymbolBuffer structure — the CPU→GPU data exchange format:
   - Must contain all data the GPU needs to reconstruct a frame WITHOUT
     re-reading the bitstream
   - Organized for efficient GPU upload (prefer SoA over AoS for GPU cache)
   - Include: partition info, prediction modes, reference indices, motion vectors,
     quantized coefficients, transform types/sizes, filter parameters,
     segmentation map, film grain parameters

2. GPU thread state machine:
   IDLE → WAITING_FOR_SYMBOLS → UPLOADING → BUILDING_CMDBUF →
   SUBMITTED → WAITING_FENCE → FRAME_READY → (back to IDLE)

3. Double-buffering strategy:
   - Two symbol buffers per pipeline slot (ping-pong)
   - CPU fills buffer A while GPU processes buffer B
   - Synchronization: per-buffer semaphore

4. What the GPU command buffer contains (abstract, not API-specific):
   - Pass 1: Inverse quantization + inverse transform (compute)
   - Pass 2: Intra prediction (compute, depends on reconstructed neighbors)
   - Pass 3: Inter prediction (compute + texture sample from ref frames)
   - Pass 4: Combine prediction + residual → reconstructed samples
   - Pass 5: Loop filter (compute)
   - Pass 6: CDEF (compute)
   - Pass 7: Loop restoration (compute)
   - Pass 8: Film grain synthesis (compute)

5. GPU memory layout:
   - Reconstructed frame buffers (DPB equivalent on GPU side)
   - Reference frames (persistent across frames)
   - Symbol upload staging buffer
   - Intermediate buffers (prediction, residual)

6. Fallback: If GPU is busy, frame can be decoded on CPU path instead
   (graceful degradation). Design the decision point.

This is a DESIGN DOCUMENT. No shader code. No API-specific calls.
Focus on data layout, threading, and synchronization.
```

### Verification checklist for Prompt 8:
- [ ] Symbol buffer contains ALL data needed — no bitstream re-reading
- [ ] Double buffering prevents CPU→GPU stalls
- [ ] GPU pipeline passes are in correct dependency order
- [ ] Intra prediction dependency (needs reconstructed neighbors) is addressed
- [ ] Reference frames in GPU memory are managed (not re-uploaded every frame)
- [ ] Fallback to CPU path is clean (no GPU-specific state leaks)

---

## Prompt 9: Integration Test Plan

```
Write a test plan and test harness design for the complete AV1 decoder API.

Test categories:

1. Unit tests (per component):
   - Pool allocator: alloc/free patterns, exhaustion, alignment
   - Job queue: SPSC/MPSC correctness, full/empty, wraparound
   - Memory calc: known resolutions produce expected ranges
   - Copy thread: correctness, multiple frames, timeout

2. API integration tests:
   - Happy path: query → create → decode 100 frames → sync/output each → flush → destroy
   - Queue full: decode queue_depth+1 frames without sync → get QUEUE_FULL → drain → continue
   - Show existing frame: verify it completes without worker dispatch
   - Flush mid-stream: flush after 10 frames, drain remaining
   - Double destroy: verify second destroy is harmless (or returns error)
   - Bad parameters: NULL pointers, zero queue_depth, insufficient memory

3. Stress tests:
   - Rapid decode/sync alternation (single frame at a time)
   - Maximum queue depth with 4K 10-bit (memory pressure)
   - All workers busy + queue full + flush simultaneously

4. Conformance:
   - Decode AV1 test vectors through new API
   - Compare output against reference decoder output (PSNR = infinity)
   - Test: AV1 test suite (aom_test) adapted to use new API

Write the test harness as a C program skeleton:
- Read .ivf or .obu file
- Parse sequence header for av1_query_memory
- Create decoder
- Loop: read AU → av1_decode → check sync → set_output → receive_output → write .y4m
- Flush and cleanup
- Compare output against known-good .y4m
```

### Verification checklist for Prompt 9:
- [ ] Happy path test exercises all 7 API functions
- [ ] Edge cases (queue full, flush, timeout) are covered
- [ ] Conformance test compares against known-good output
- [ ] Test harness can read standard .ivf files
- [ ] Stress tests run clean under ThreadSanitizer and AddressSanitizer

---

## Prompt Execution Order

```
Prompt 1 (allocator) ──→ Prompt 2 (queues) ──→ Prompt 3 (memory calc)
                                                       │
Prompt 4 (copy thread) ←──────────────────────────────┘
       │
       ├──→ Prompt 5 (OBU split design)
       │         │
       │         ▼
       ├──→ Prompt 6 (create_decoder)
       │         │
       │         ▼
       └──→ Prompt 7 (sync/flush/destroy)
                 │
                 ▼
            Prompt 8 (GPU thread design)
                 │
                 ▼
            Prompt 9 (integration tests)
```

Prompts 1–4 produce compilable, testable code.
Prompts 5–7 produce design pseudocode requiring AOM source modification.
Prompt 8 produces GPU design documentation.
Prompt 9 produces a test plan and harness skeleton.
