# Implementation Prompts for MiniMax-M2.5

These prompts are designed to be fed sequentially to MiniMax-M2.5. Each prompt produces a verifiable artifact. The outputs of earlier prompts become context/inputs for later ones.

**Workflow**: Run prompt → verify output → correct if needed → feed corrected output into next prompt.

**Key constraints** (see `00-decisions.md`):
- Queue depth is serial (no parallel frame decode)
- AU = TD-to-TD (all OBUs between temporal delimiters)
- malloc override is acceptable (no custom allocator needed initially)
- 200µs budget is a future optimization, not a requirement now
- GPU thread is about CPU offload, not raw speed
- Film grain is a GPU shader writing directly to dst with format conversion

---

## Prompt 1: malloc/memalign/free Override Layer

```
You are implementing a memory override layer for the AOM AV1 reference decoder.

The goal: redirect all aom_malloc / aom_memalign / aom_free calls to allocate
from a single contiguous memory block provided by the caller at decoder creation.

This is NOT a full custom allocator rewrite. We are overriding the existing
AOM allocation functions so all internal allocations route through our block.

File: av1_mem_override.h / av1_mem_override.c

Approach:
1. The caller provides a memory block (base pointer + size) at create time.
2. We implement a simple bump allocator + free-list on top of this block.
3. We override aom_malloc, aom_memalign, aom_calloc, aom_free by replacing
   the implementations in aom_mem/aom_mem.c (or via function pointers).

Functions:
- av1_mem_init(void *base, size_t size)
  Initialize the allocator state at the start of the memory block.
  Uses a small header region for bookkeeping.

- void *av1_mem_malloc(size_t size)
  Allocate from the block. Simple bump with free-list fallback.

- void *av1_mem_memalign(size_t align, size_t size)
  Aligned allocation from the block.

- void *av1_mem_calloc(size_t num, size_t size)
  Zero-initialized allocation.

- void av1_mem_free(void *ptr)
  Return to free-list (or no-op for bump-only mode).

- size_t av1_mem_query_size(const Av1StreamInfo *info, int queue_depth, int num_workers)
  Estimate total memory needed. This drives av1_query_memory().
  Use the formulas below:

  frame_size = ALIGN64(width) × ALIGN64(height) × bps × chroma_factor
  dpb_count  = 8 + queue_depth + 1
  dpb_total  = dpb_count × (frame_size + overhead_per_frame)
  scratch    = num_workers × PER_WORKER_SCRATCH
  overhead   = decoder_context + entropy_contexts + tables
  total      = dpb_total + scratch + overhead + 10% headroom

Integration with AOM:
- In aom_mem/aom_mem.c, the existing functions call our overrides when
  a global "use_pool" flag is set (set during av1_create_decoder).
- When use_pool is false (e.g., encoder path), original malloc behavior is used.

Requirements:
- Thread-safe (multiple worker threads may allocate concurrently during decode)
- Use a mutex for the allocator (simplicity over speed — this is reference code)
- Track peak usage for debugging
- Write a test that: creates a 256MB block, inits the allocator, performs 1000
  random-sized allocations (1 byte to 1MB) with random alignments (1/4/16/64),
  frees half of them, allocates 500 more, prints peak usage and fragmentation.
```

### Verification checklist for Prompt 1:
- [ ] All allocations come from the provided block (verify no malloc calls via ltrace/strace)
- [ ] Aligned allocations are actually aligned (test with 64-byte alignment)
- [ ] Thread-safe under concurrent allocations (test with 4 threads)
- [ ] Free + re-allocate works (freed memory can be reused)
- [ ] Query size produces reasonable numbers for 1080p and 4K
- [ ] Compiles with: `gcc -std=c11 -Wall -Wextra -pthread`

---

## Prompt 2: Job Queue (Simple Mutex-Based)

```
Implement a simple thread-safe job queue for the AV1 decoder.

Since the pipeline is SERIAL (one frame decoded at a time), this queue holds
completed frames waiting for the application to pick them up via av1_sync()
and eventually av1_set_output() / av1_receive_output().

File: av1_job_queue.h / av1_job_queue.c

This is a simple fixed-size circular buffer protected by a mutex + condvar.
No need for lock-free — this is reference code prioritizing correctness.

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

Functions:
- av1_frame_queue_init(Av1FrameQueue *q, Av1FrameEntry *storage, int capacity)
- int av1_frame_queue_push(Av1FrameQueue *q, const Av1FrameEntry *entry)
  Returns 0 on success, -1 if full (non-blocking).
- int av1_frame_queue_pop(Av1FrameQueue *q, Av1FrameEntry *out, uint32_t timeout_us)
  Returns 0 on success, -1 on timeout. timeout_us=0 means non-blocking poll.
  Uses pthread_cond_timedwait for timeout > 0.
- int av1_frame_queue_count(Av1FrameQueue *q)
- int av1_frame_queue_is_full(Av1FrameQueue *q)
- void av1_frame_queue_destroy(Av1FrameQueue *q)

Write a test that:
  - Creates a queue with capacity 8
  - Pushes 8 entries, verifies 9th push returns -1 (full)
  - Pops 3, verifies correct FIFO order
  - Pushes 3 more, pops all 8, verifies order
  - Tests timeout: pop on empty queue with 100ms timeout returns -1
  - Two-thread test: producer pushes 1000 entries with small delays,
    consumer pops 1000 entries. Verify all received in order.
```

### Verification checklist for Prompt 2:
- [ ] FIFO order preserved
- [ ] Full detection works (push returns -1)
- [ ] Timeout works (pop returns -1 after specified time)
- [ ] Non-blocking poll (timeout=0) returns immediately
- [ ] Two-thread stress test passes under ThreadSanitizer
- [ ] Destroy cleans up mutex/condvar

---

## Prompt 3: av1_query_memory and av1_create_decoder

```
Implement av1_query_memory and av1_create_decoder for the AV1 decoder.

Context:
- The decoder pipeline is SERIAL — one frame decoded at a time.
- Queue depth = how many decoded frames can wait for output pickup.
- An AU is defined as all OBUs from one Temporal Delimiter to the next.
- Memory allocation uses the override layer from Prompt 1 (malloc redirect).
- Worker threads, copy thread, and optionally a GPU thread are created here.

File: av1_decoder_api.h / av1_decoder_api.c

### av1_query_memory

Input: Av1StreamInfo (max_width, max_height, bit_depth, chroma_subsampling,
       monochrome) + queue_depth + num_worker_threads

Output: Av1MemoryRequirements { total_size, alignment, breakdown... }

Implementation:
  1. Compute frame buffer size (with 128-pixel border for MC)
  2. Compute DPB: (8 + queue_depth + 1) frame buffers
  3. Compute per-worker scratch (MC temp, convolution, OBMC buffers)
  4. Compute entropy context storage (queue_depth × ~75KB)
  5. Compute decoder context + tile data + mode info grid
  6. Sum with 10% headroom, 64-byte alignment requirement

### av1_create_decoder

Input: Av1DecoderConfig { memory_base, memory_size, queue_depth,
       num_worker_threads, thread priorities/affinities, use_gpu, gpu_device }

Output: Av1Decoder handle

Implementation:
  1. Validate: memory_size >= what query_memory would return
  2. Call av1_mem_init(memory_base, memory_size) to set up allocator
  3. Set global "use_pool" flag so aom_malloc routes to our block
  4. Allocate and zero-init the Av1Decoder struct (via redirected aom_memalign)
  5. Initialize internal AOM state:
     - Call the existing AOM decoder init path (decoder_init from av1_dx_iface.c)
     - This internally allocates AV1Decoder, AV1_COMMON, BufferPool, etc.
       through our redirected malloc
  6. Initialize the ready queue (Av1FrameQueue from Prompt 2)
  7. Create worker threads (reuse AOM's existing tile worker creation)
  8. Create copy thread
  9. Optionally create GPU thread stub (if use_gpu=1)
  10. Set state to CREATED
  11. Return handle

Error handling:
  - If any step fails, clean up everything done so far
  - Join any threads already created
  - Return appropriate error code

Write a test that:
  - Queries memory for 1080p 8-bit 4:2:0, queue_depth=4, 4 workers
  - Allocates the memory (aligned_alloc)
  - Creates the decoder
  - Verifies decoder handle is non-NULL
  - Destroys (placeholder — just free for now)
  - Prints memory breakdown
```

### Verification checklist for Prompt 3:
- [ ] Query returns reasonable sizes (1080p ~150-300MB, 4K ~1-2GB)
- [ ] Create succeeds when given enough memory
- [ ] Create fails gracefully when given insufficient memory
- [ ] All AOM internal state is initialized (no crashes on subsequent decode)
- [ ] Threads are created and running (verify with pthread or ps)
- [ ] No system malloc calls during create (all through override)

---

## Prompt 4: Copy Thread

```
Implement the copy thread for the AV1 decoder.

The copy thread services SET OUTPUT / RECEIVE OUTPUT. When the app provides
a destination buffer via av1_set_output(), a copy job is enqueued. The copy
thread copies plane-by-plane and signals completion.

In GPU mode (stretch goal), the copy thread instead dispatches a GPU shader
that performs film grain synthesis + format conversion + copy in one pass.
For now, implement CPU-only copy. The GPU path is a future extension point.

File: av1_copy_thread.h / av1_copy_thread.c

Structures:
typedef struct Av1CopyJob {
    uint32_t frame_id;
    int      dpb_slot;
    // Source (internal DPB frame buffer)
    const uint8_t *src_planes[3];
    int      src_strides[3];
    // Destination (caller-provided buffer)
    uint8_t *dst_planes[3];
    int      dst_strides[3];
    // Dimensions
    int      plane_widths[3];    // copy width in bytes per plane
    int      plane_heights[3];   // rows per plane
    // Status
    _Atomic int status;          // PENDING=0, IN_PROGRESS=1, COMPLETE=2
} Av1CopyJob;

Thread loop:
  1. Wait on condvar for a job
  2. Dequeue job, set status = IN_PROGRESS
  3. For each plane (Y, U, V):
       For each row:
         memcpy(dst_row, src_row, width_bytes)
  4. Set status = COMPLETE (atomic store)
  5. Broadcast completion condvar

av1_set_output():
  - Populate an Av1CopyJob from the DPB slot and caller's Av1OutputBuffer
  - Enqueue to copy thread

av1_receive_output():
  - Wait until the job for frame_id has status == COMPLETE
  - Release the DPB slot reference (decrease_ref_count)
  - Return AV1_OK

Write a test that:
  - Creates a copy thread
  - Fills a 1920×1080 YUV420 source with a known pattern
  - Provides a zeroed destination buffer
  - Enqueues a copy, waits for completion
  - Verifies dst == src byte-for-byte
  - Tests: enqueue 4 copies back-to-back, wait for all
  - Tests: destroy thread while idle (clean shutdown)
  - Tests: destroy thread while copy is in-flight (waits for current job)
```

### Verification checklist for Prompt 4:
- [ ] Byte-identical copy (memcmp src vs dst)
- [ ] Multiple sequential copies work
- [ ] Clean shutdown (no thread leak, no hang)
- [ ] ThreadSanitizer clean
- [ ] Atomic status transitions are correct

---

## Prompt 5: av1_decode — Serial Decode with AU = TD-to-TD

```
Implement av1_decode() for the AV1 decoder.

Key constraints:
- The pipeline is SERIAL — decode one frame at a time, fully, before accepting
  the next DECODE call. (Async decode is a future optimization.)
- An AU is all OBUs from one Temporal Delimiter to the next.
- If the ready queue is full (queue_depth frames waiting for pickup),
  return AV1_QUEUE_FULL.
- On success, the decoded frame is placed in the ready queue for av1_sync()
  to pick up.

File: av1_decoder_api.c (add to existing)

av1_decode(decoder, data, data_size, out_result):
  1. If decoder state != CREATED and != DECODING → return INVALID_PARAM
  2. Set state = DECODING
  3. If ready queue is full → return AV1_QUEUE_FULL
  4. Call the existing AOM decode path:
     - This is the existing aom_codec_decode() internally, which calls
       aom_decode_frame_from_obus() → av1_decode_frame_headers_and_setup()
       → av1_decode_tg_tiles_and_wrapup()
     - For now, this runs SYNCHRONOUSLY on the caller's thread. That's fine.
  5. After decode completes:
     - If show_frame or show_existing_frame:
       a. Assign a frame_id (monotonic counter)
       b. Push {frame_id, dpb_slot, show_frame, show_existing} to ready queue
       c. Fill out_result: frame_ready=1, frame_id, show_existing_frame flag
     - If !show_frame (non-displayed reference-only frame):
       a. Fill out_result: frame_ready=0
  6. Return AV1_OK

How to integrate with AOM internals:
  - We wrap the existing AOM codec interface. Internally, the Av1Decoder holds
    an aom_codec_ctx_t that we initialized in av1_create_decoder.
  - av1_decode() calls aom_codec_decode() on that internal context.
  - After decode, we check if aom_codec_get_frame() returns a frame.
  - If it does, we grab the YV12_BUFFER_CONFIG pointer and DPB slot info
    and enqueue to our ready queue.

Show_existing_frame handling:
  - AOM handles this internally — aom_codec_decode() will produce an output
    frame via aom_codec_get_frame() for show_existing. No special path needed
    on our side. We just check if a frame came out.

Write a test that:
  - Creates decoder (from Prompt 3)
  - Reads a .ivf file (provide an IVF parser: read header, then read frames)
  - Calls av1_decode() for each frame
  - Checks out_result.frame_ready
  - For now, don't do set_output/receive_output — just verify frames are
    queued and queue_full works when we don't drain
  - Decode queue_depth+1 frames without draining → verify QUEUE_FULL on last
```

### Verification checklist for Prompt 5:
- [ ] Decodes AV1 bitstream without crashing
- [ ] Frame ready flag set correctly for show_frame / show_existing
- [ ] Non-display frames (reference only) produce frame_ready=0
- [ ] QUEUE_FULL returned when ready queue is full
- [ ] Frame IDs are monotonically increasing
- [ ] State transitions correct (CREATED → DECODING)

---

## Prompt 6: av1_sync, av1_set_output, av1_receive_output

```
Implement the output retrieval pipeline.

av1_sync(decoder, timeout_us, out_result):
  1. Pop from ready queue (using timeout from Prompt 2's queue)
  2. If got a frame: fill out_result, return AV1_OK
  3. If timeout and no frame: return AV1_NEED_MORE_DATA
  4. If state==FLUSHING and ready queue empty: return AV1_END_OF_STREAM

av1_set_output(decoder, frame_id, output_buffer):
  1. Look up the frame by frame_id (it was dequeued by av1_sync into a
     "pending output" slot or lookup table)
  2. Get the DPB slot's YV12_BUFFER_CONFIG (source planes, strides)
  3. Build an Av1CopyJob from source + output_buffer destination
  4. Enqueue to copy thread (from Prompt 4)
  5. Return AV1_OK

av1_receive_output(decoder, frame_id, timeout_us):
  1. Wait for the copy job with this frame_id to reach status COMPLETE
  2. Release the DPB reference (ref_count--)
  3. Return AV1_OK (or timeout error)

Data flow:
  av1_decode → ready_queue → av1_sync → pending_output → av1_set_output
  → copy_thread → av1_receive_output → done, DPB slot freed

The "pending output" is a small array (queue_depth entries) mapping
frame_id → dpb_slot + copy_job. av1_sync moves entries from ready_queue
to this array. av1_set_output triggers the copy. av1_receive_output
waits and cleans up.

Write a test (end-to-end):
  - Create decoder
  - Read .ivf file, decode each frame
  - After each av1_decode that returns frame_ready:
    - av1_sync (should return immediately since we just decoded)
    - av1_set_output with a malloc'd YUV buffer
    - av1_receive_output (blocks until copy done)
    - Write output to .y4m file
  - Compare output .y4m against reference (decoded by aomdec)
  - This is the CONFORMANCE test — output must be bit-exact
```

### Verification checklist for Prompt 6:
- [ ] Full decode → sync → set_output → receive_output pipeline works
- [ ] Output is bit-exact with aomdec reference output
- [ ] Multiple frames decoded and output correctly
- [ ] DPB slots are freed after receive_output (no leak / exhaustion)
- [ ] Timeout works on av1_sync and av1_receive_output

---

## Prompt 7: av1_flush and av1_destroy_decoder

```
Implement the cleanup functions.

av1_flush(decoder):
  1. Set state = FLUSHING
  2. The serial pipeline means there's no in-flight async work to drain.
     All decoded frames are already in the ready queue.
  3. Reject any subsequent av1_decode() calls (return error).
  4. The application must drain remaining frames via av1_sync → av1_set_output
     → av1_receive_output until av1_sync returns AV1_END_OF_STREAM.
  5. Return AV1_OK

av1_destroy_decoder(decoder):
  1. If not flushed, flush first (or just force-drain)
  2. Wait for any in-progress copy to complete
  3. Signal copy thread to exit, join it
  4. Signal worker threads to exit, join them (reuse AOM's thread cleanup)
  5. If GPU thread exists, signal and join it
  6. Destroy internal AOM codec context (aom_codec_destroy)
  7. Destroy mutexes, condvars
  8. Zero the decoder struct (security wipe)
  9. Clear the "use_pool" flag (restore normal malloc)
  10. Return AV1_OK
  11. Caller may now free the memory block

Error states to handle:
  - Destroy while copy in progress → wait for copy, then destroy
  - Destroy without prior flush → implicit flush + discard remaining frames
  - Double destroy → return error or no-op

Write a test:
  - Normal: decode 10 frames → flush → drain all → destroy → free memory
  - Early destroy: decode 5 frames → destroy without flush → verify no hang/leak
  - Empty: create → destroy immediately (no decodes) → verify clean
```

### Verification checklist for Prompt 7:
- [ ] Flush + drain cycle works (END_OF_STREAM returned correctly)
- [ ] Destroy after flush is clean
- [ ] Destroy without flush doesn't hang
- [ ] Destroy with no decodes doesn't crash
- [ ] No thread leaks (all threads joined)
- [ ] AddressSanitizer clean (no leaks)

---

## Prompt 8: GPU Thread Stub

```
Implement a GPU thread STUB for the AV1 decoder.

This is NOT a GPU implementation. It's the thread lifecycle and data flow
infrastructure that a real GPU implementation would plug into.

The GPU thread's purpose: free the CPU from reconstruction + filtering work
so the CPU is available for game logic.

File: av1_gpu_thread.h / av1_gpu_thread.c

The GPU thread:
  1. Receives a "GPU job" after CPU entropy decode completes
  2. In the stub: just marks the job as complete after a simulated delay
  3. In a real implementation: would build command buffers, submit to GPU,
     wait on fence, then mark complete

For the FILM GRAIN path (real design, not stub):
  - The GPU thread dispatches a compute shader that:
    a. Reads the un-grained reconstructed frame from DPB (GPU memory)
    b. Synthesizes film grain per the AV1 spec
    c. Optionally converts pixel format (e.g., planar YUV → NV12, 
       10-bit → packed, etc.) based on a "dst texture descriptor"
    d. Writes directly to the destination buffer provided by SET OUTPUT
  - This means for GPU mode, SET OUTPUT provides a GPU-visible buffer
    (or a descriptor for the target surface)
  - The copy thread is NOT used in GPU mode — the grain shader IS the copy

Structures:
typedef struct Av1GpuJob {
    uint32_t frame_id;
    int      dpb_slot;
    int      needs_film_grain;
    // Film grain params (from frame header)
    // Destination descriptor (from av1_set_output)
    void    *dst_descriptor;     // opaque — GPU API specific
    _Atomic int status;          // PENDING, PROCESSING, COMPLETE
} Av1GpuJob;

typedef struct Av1GpuThread {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    Av1GpuJob *jobs;
    int capacity;
    int head, tail, count;
    int exit_flag;
    void *gpu_device;            // opaque GPU device handle
} Av1GpuThread;

GPU thread loop (STUB):
  while (!exit) {
    wait for job
    dequeue job
    // STUB: simulate GPU work
    usleep(1000);  // pretend 1ms of GPU processing
    set status = COMPLETE
    signal completion
  }

IMPORTANT: Document clearly where a real implementation would:
  - Upload symbol data (coefficients, modes, MVs)
  - Build reconstruction command buffer
  - Build filter command buffer
  - Build film grain + format conversion + output shader
  - Submit and wait on fence
  Use comments like: // GPU_IMPL: build inverse transform compute dispatch here

Write a test:
  - Create GPU thread stub
  - Enqueue 10 fake jobs
  - Wait for each to complete
  - Verify all complete in order
  - Destroy cleanly
```

### Verification checklist for Prompt 8:
- [ ] Thread starts and stops cleanly
- [ ] Jobs complete in FIFO order
- [ ] Stub delay works (each job takes ~1ms)
- [ ] Clean shutdown with in-flight jobs
- [ ] Comments clearly mark all GPU_IMPL extension points
- [ ] Film grain + format conversion + direct-to-dst path is documented

---

## Prompt 9: End-to-End Integration Test

```
Write a complete end-to-end test program for the AV1 decoder API.

This program:
  1. Opens an .ivf file
  2. Parses the IVF header to get width/height
  3. Parses the first frame to extract the AV1 Sequence Header OBU
     (to get bit_depth, chroma subsampling, etc.)
  4. Calls av1_query_memory() with stream info
  5. Allocates the memory block
  6. Calls av1_create_decoder()
  7. Decode loop:
     - Read next AU from IVF (frame = all bytes for one IVF frame entry)
     - Call av1_decode()
     - If QUEUE_FULL: drain via sync → set_output → receive_output, retry
     - If frame_ready: sync → set_output → receive_output → write to .y4m
  8. av1_flush()
  9. Drain remaining: sync → set_output → receive_output until END_OF_STREAM
  10. av1_destroy_decoder()
  11. Free memory

IVF format (for the parser):
  - 32 bytes header: signature "DKIF", version, header_size, fourcc,
    width, height, timebase_num, timebase_den, num_frames, unused
  - Per frame: 4 bytes size (LE) + 8 bytes timestamp (LE) + size bytes data

Y4M output format:
  - Header: "YUV4MPEG2 W{w} H{h} F{fps_num}:{fps_den} Ip C{colorspace}\n"
  - Per frame: "FRAME\n" + Y plane + U plane + V plane

Conformance verification:
  - Decode a known test vector with aomdec to produce reference.y4m
  - Decode same vector with our API to produce test.y4m
  - Binary diff: they must be identical

Test vectors to use:
  - AV1 test suite vectors from aomedia.org (or any .ivf file)

Also test error paths:
  - Truncated bitstream (pass fewer bytes than IVF says)
  - Zero-length AU
  - QUEUE_FULL recovery
```

### Verification checklist for Prompt 9:
- [ ] Successfully decodes at least one AV1 .ivf test vector
- [ ] Output matches aomdec bit-exactly
- [ ] QUEUE_FULL handling works (drain + retry succeeds)
- [ ] Flush drains all remaining frames
- [ ] No memory leaks (AddressSanitizer clean)
- [ ] No thread leaks (all threads joined at exit)
- [ ] IVF parser handles edge cases (empty file, truncated frame)

---

## Prompt Execution Order

```
Prompt 1 (malloc override) → Prompt 2 (job queue) → Prompt 3 (query + create)
                                                           │
Prompt 4 (copy thread) ←──────────────────────────────────┘
       │
       ├──→ Prompt 5 (av1_decode)
       │         │
       │         ▼
       ├──→ Prompt 6 (sync + set_output + receive_output)
       │         │
       │         ▼
       └──→ Prompt 7 (flush + destroy)
                 │
                 ▼
            Prompt 8 (GPU thread stub)
                 │
                 ▼
            Prompt 9 (end-to-end integration test)
```

All prompts produce compilable, testable code.
Prompt 8 is a stub with documented extension points for real GPU work.
Prompt 9 is the conformance gate — bit-exact match with aomdec.
