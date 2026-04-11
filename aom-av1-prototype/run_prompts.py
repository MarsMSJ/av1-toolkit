#!/usr/bin/env python3
"""
Overnight batch runner for AV1 decoder API implementation prompts.
Sends each prompt sequentially to MiniMax-M2.5 on a local vLLM cluster,
feeding prior outputs as context to subsequent prompts.

Usage:
    python run_prompts.py --base-url http://sparks:8000/v1
    python run_prompts.py --base-url http://sparks:8000/v1 --start-from 3
    python run_prompts.py --base-url http://sparks:8000/v1 --only 5
    python run_prompts.py --dry-run   # print prompts without sending
"""

import argparse
import json
import os
import sys
import time
import textwrap
import hashlib
from datetime import datetime
from pathlib import Path

try:
    import requests
except ImportError:
    print("ERROR: 'requests' not installed. Run: pip install requests")
    sys.exit(1)

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

SCRIPT_DIR = Path(__file__).parent.resolve()
OUTPUT_DIR = SCRIPT_DIR / "prompt_outputs"
MODEL_NAME = "MiniMax-M2.5"  # vLLM will serve whatever model is loaded;
                               # this is just for the API "model" field.
                               # Change if your vLLM names it differently.

MAX_TOKENS = 16384  # M2.5 supports long output; bump if truncated
TEMPERATURE = 0.2   # low temp for code generation
REQUEST_TIMEOUT = 3600  # 60 min per prompt — full-weight model is slow for long output
MAX_CONTINUATIONS = 3   # max follow-up requests if output is truncated

# ---------------------------------------------------------------------------
# AOM source files to inject as context (~70-90K tokens total, fits in 197K)
# Paths are relative to the repo root (av1-toolkit/)
# ---------------------------------------------------------------------------

# Which AOM source files each prompt needs as reference context.
# "all" = shared base set; per-prompt overrides add more.
AOM_BASE_DIR_CANDIDATES = [
    Path.home() / "dev-tools" / "av1-toolkit" / "aom",   # spark cluster
    SCRIPT_DIR.parent / "aom",                             # sibling to aom-av1-prototype/
]

AOM_CONTEXT_FILES = {
    # Core files every prompt should see
    "core": [
        "aom_mem/aom_mem.h",
        "aom_mem/aom_mem.c",
        "aom/aom_codec.h",
        "aom/aom_decoder.h",
    ],
    # Decoder internals — for prompts that need struct definitions
    "decoder_structs": [
        "av1/decoder/decoder.h",
        "av1/common/av1_common_int.h",
    ],
    # Decoder implementation — for prompts that modify the decode path
    "decoder_impl": [
        "aom/src/aom_decoder.c",
        "av1/decoder/decoder.c",
        "av1/av1_dx_iface.c",
    ],
    # OBU / decode pipeline — for the decode split
    "decode_pipeline": [
        "av1/decoder/obu.c",
    ],
    # Threading
    "threading": [
        "av1/common/thread_common.h",
    ],
}

# Map prompt_id -> which context groups to include
PROMPT_CONTEXT_MAP = {
    1: ["core"],                                                    # mem override
    2: ["core"],                                                    # job queue
    3: ["core", "decoder_structs", "decoder_impl"],                 # query + create
    4: ["core", "decoder_structs"],                                 # copy thread
    5: ["core", "decoder_structs", "decoder_impl", "decode_pipeline"],  # av1_decode
    6: ["core", "decoder_structs"],                                 # sync/output
    7: ["core", "decoder_structs", "decoder_impl"],                 # flush/destroy
    8: ["core", "decoder_structs", "threading"],                    # GPU thread stub
    9: ["core", "decoder_structs", "decoder_impl", "decode_pipeline"],  # e2e test
}


def find_aom_base() -> Path:
    """Find the AOM source directory."""
    for candidate in AOM_BASE_DIR_CANDIDATES:
        if candidate.is_dir() and (candidate / "aom" / "aom_decoder.h").exists():
            return candidate
    return None


def load_aom_context(prompt_id: int, aom_base: Path) -> str:
    """Load AOM source files relevant to this prompt and format as context."""
    if aom_base is None:
        return ""

    groups = PROMPT_CONTEXT_MAP.get(prompt_id, ["core"])
    files_to_load = []
    seen = set()
    for group in groups:
        for f in AOM_CONTEXT_FILES.get(group, []):
            if f not in seen:
                files_to_load.append(f)
                seen.add(f)

    sections = []
    total_chars = 0
    for rel_path in files_to_load:
        full_path = aom_base / rel_path
        if not full_path.exists():
            sections.append(f"// FILE NOT FOUND: {rel_path}")
            continue
        content = full_path.read_text(encoding="utf-8", errors="replace")
        total_chars += len(content)
        sections.append(f"### {rel_path}\n```c\n{content}\n```")

    if not sections:
        return ""

    header = (
        f"## AOM Reference Decoder Source Code\n\n"
        f"Below are the relevant AOM source files ({len(files_to_load)} files, "
        f"~{total_chars // 1000}K chars). Use these as reference for struct definitions, "
        f"function signatures, and the existing implementation you are wrapping/modifying.\n\n"
    )
    return header + "\n\n".join(sections)

# ---------------------------------------------------------------------------
# System prompt (shared context for all prompts)
# ---------------------------------------------------------------------------

SYSTEM_PROMPT = """\
You are a senior C systems programmer. You are NOT an agent — you have NO \
tools, NO file access, NO ability to read files or run commands. You can \
ONLY output text. Do NOT output any XML tool calls or attempt to invoke any tools.

Your task: generate complete, compilable C11 source files. Output the FULL \
file contents directly in your response using markdown fenced code blocks. \
Each file must be preceded by a heading with its filename, like:

### av1_example.h
```c
// full file contents here
```

You are implementing components for an AV1 video decoder.

Key project constraints:
- Pipeline is SERIAL — one frame decoded at a time, queue depth is for \
  buffering decoded frames awaiting output pickup.
- An Access Unit (AU) = all OBUs from one Temporal Delimiter to the next.
- Memory: override aom_malloc/aom_memalign/aom_free to allocate from a \
  caller-provided contiguous block. No need for a from-scratch allocator.
- 200us DECODE budget is a future optimization — synchronous decode is fine.
- GPU thread is a stub for CPU offload; film grain will be a GPU compute \
  shader writing directly to the destination buffer with format conversion.
- Target: AOM reference decoder (libaom). This is for accuracy, not speed.
- AOM source tree has headers in aom/, av1/, aom_dsp/, aom_mem/.
- Use pthreads for threading. C11 atomics where appropriate.
- Always include a compilable test with main().
- Output COMPLETE files — headers and implementation — ready to save and compile.
- Do NOT try to read files. Do NOT output tool calls. Just write the code.
"""

# ---------------------------------------------------------------------------
# The 9 prompts, extracted and self-contained
# ---------------------------------------------------------------------------

PROMPTS = [
    # -----------------------------------------------------------------------
    # Prompt 1
    # -----------------------------------------------------------------------
    {
        "id": 1,
        "name": "malloc_override",
        "title": "malloc/memalign/free Override Layer",
        "depends_on": [],
        "prompt": textwrap.dedent("""\
            You are implementing a memory override layer for the AOM AV1 reference decoder.

            The goal: redirect all aom_malloc / aom_memalign / aom_free calls to allocate
            from a single contiguous memory block provided by the caller at decoder creation.

            This is NOT a full custom allocator rewrite. We are overriding the existing
            AOM allocation functions so all internal allocations route through our block.

            Produce two files: av1_mem_override.h and av1_mem_override.c

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
              Use the formulas:
              frame_size = ALIGN64(width) * ALIGN64(height) * bps * chroma_factor
              dpb_count  = 8 + queue_depth + 1
              dpb_total  = dpb_count * (frame_size + overhead_per_frame)
              scratch    = num_workers * PER_WORKER_SCRATCH
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
            - Write a test main() that: creates a 256MB block, inits the allocator, performs
              1000 random-sized allocations (1 byte to 1MB) with random alignments (1/4/16/64),
              frees half of them, allocates 500 more, prints peak usage and fragmentation.

            Output the complete av1_mem_override.h, av1_mem_override.c, and test_mem_override.c
            files ready to compile with: gcc -std=c11 -Wall -Wextra -pthread -o test test_mem_override.c av1_mem_override.c
        """),
        "checklist": [
            "All allocations come from the provided block (no malloc calls)",
            "Aligned allocations are actually aligned (test with 64-byte alignment)",
            "Thread-safe under concurrent allocations (test with 4 threads)",
            "Free + re-allocate works (freed memory can be reused)",
            "Query size produces reasonable numbers for 1080p and 4K",
            "Compiles with: gcc -std=c11 -Wall -Wextra -pthread",
        ],
    },
    # -----------------------------------------------------------------------
    # Prompt 2
    # -----------------------------------------------------------------------
    {
        "id": 2,
        "name": "job_queue",
        "title": "Job Queue (Simple Mutex-Based)",
        "depends_on": [],
        "prompt": textwrap.dedent("""\
            Implement a simple thread-safe job queue for the AV1 decoder.

            Since the pipeline is SERIAL (one frame decoded at a time), this queue holds
            completed frames waiting for the application to pick them up via av1_sync()
            and eventually av1_set_output() / av1_receive_output().

            Produce two files: av1_job_queue.h and av1_job_queue.c

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

            Write a test (test_job_queue.c) that:
              - Creates a queue with capacity 8
              - Pushes 8 entries, verifies 9th push returns -1 (full)
              - Pops 3, verifies correct FIFO order
              - Pushes 3 more, pops all 8, verifies order
              - Tests timeout: pop on empty queue with 100ms timeout returns -1
              - Two-thread test: producer pushes 1000 entries with small delays,
                consumer pops 1000 entries. Verify all received in order.

            Output complete av1_job_queue.h, av1_job_queue.c, and test_job_queue.c.
        """),
        "checklist": [
            "FIFO order preserved",
            "Full detection works (push returns -1)",
            "Timeout works (pop returns -1 after specified time)",
            "Non-blocking poll (timeout=0) returns immediately",
            "Two-thread stress test passes under ThreadSanitizer",
            "Destroy cleans up mutex/condvar",
        ],
    },
    # -----------------------------------------------------------------------
    # Prompt 3
    # -----------------------------------------------------------------------
    {
        "id": 3,
        "name": "query_create",
        "title": "av1_query_memory and av1_create_decoder",
        "depends_on": [1, 2],
        "prompt": textwrap.dedent("""\
            Implement av1_query_memory and av1_create_decoder for the AV1 decoder.

            You have already implemented (provided as context above):
            - Prompt 1 output: av1_mem_override.h/.c — memory override allocator
            - Prompt 2 output: av1_job_queue.h/.c — frame queue

            Context:
            - The decoder pipeline is SERIAL — one frame decoded at a time.
            - Queue depth = how many decoded frames can wait for output pickup.
            - An AU is defined as all OBUs from one Temporal Delimiter to the next.
            - Memory allocation uses the override layer from Prompt 1 (malloc redirect).
            - Worker threads, copy thread, and optionally a GPU thread are created here.

            Produce: av1_decoder_api.h and av1_decoder_api.c

            ### av1_query_memory

            Input: Av1StreamInfo (max_width, max_height, bit_depth, chroma_subsampling,
                   monochrome) + queue_depth + num_worker_threads

            Output: Av1MemoryRequirements { total_size, alignment, breakdown... }

            Implementation:
              1. Compute frame buffer size (with 128-pixel border for MC)
              2. Compute DPB: (8 + queue_depth + 1) frame buffers
              3. Compute per-worker scratch (MC temp, convolution, OBMC buffers)
              4. Compute entropy context storage (queue_depth * ~75KB)
              5. Compute decoder context + tile data + mode info grid
              6. Sum with 10% headroom, 64-byte alignment requirement

            ### av1_create_decoder

            Input: Av1DecoderConfig { memory_base, memory_size, queue_depth,
                   num_worker_threads, thread priorities/affinities, use_gpu, gpu_device }

            Output: Av1Decoder handle

            Implementation:
              1. Validate: memory_size >= what query_memory would return
              2. Call av1_mem_init(memory_base, memory_size)
              3. Set global "use_pool" flag so aom_malloc routes to our block
              4. Allocate the Av1Decoder struct (via redirected aom_memalign)
              5. Initialize internal AOM state (AV1Decoder, AV1_COMMON, BufferPool, etc.)
              6. Initialize the ready queue (Av1FrameQueue from Prompt 2)
              7. Create worker threads (reuse AOM's existing tile worker creation)
              8. Create copy thread
              9. Optionally create GPU thread stub (if use_gpu=1)
              10. Set state to CREATED, return handle

            Error handling: if any step fails, clean up everything done so far.

            Write a test (test_query_create.c) that:
              - Queries memory for 1080p 8-bit 4:2:0, queue_depth=4, 4 workers
              - Allocates the memory (aligned_alloc)
              - Creates the decoder
              - Verifies decoder handle is non-NULL
              - Destroys (placeholder — just free for now)
              - Prints memory breakdown

            Output complete av1_decoder_api.h, av1_decoder_api.c, and test_query_create.c.
        """),
        "checklist": [
            "Query returns reasonable sizes (1080p ~150-300MB, 4K ~1-2GB)",
            "Create succeeds when given enough memory",
            "Create fails gracefully when given insufficient memory",
            "All AOM internal state is initialized",
            "Threads are created and running",
            "No system malloc calls during create (all through override)",
        ],
    },
    # -----------------------------------------------------------------------
    # Prompt 4
    # -----------------------------------------------------------------------
    {
        "id": 4,
        "name": "copy_thread",
        "title": "Copy Thread",
        "depends_on": [2],
        "prompt": textwrap.dedent("""\
            Implement the copy thread for the AV1 decoder.

            You have already implemented (provided as context above):
            - Prompt 2 output: av1_job_queue.h/.c — frame queue for reference

            The copy thread services SET OUTPUT / RECEIVE OUTPUT. When the app provides
            a destination buffer via av1_set_output(), a copy job is enqueued. The copy
            thread copies plane-by-plane and signals completion.

            In GPU mode (stretch goal), the copy thread instead dispatches a GPU shader
            that performs film grain synthesis + format conversion + copy in one pass.
            For now, implement CPU-only copy. The GPU path is a future extension point.
            Mark GPU extension points with: // GPU_IMPL: ...

            Produce: av1_copy_thread.h and av1_copy_thread.c

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

            Write a test (test_copy_thread.c) that:
              - Creates a copy thread
              - Fills a 1920x1080 YUV420 source with a known pattern
              - Provides a zeroed destination buffer
              - Enqueues a copy, waits for completion
              - Verifies dst == src byte-for-byte
              - Tests: enqueue 4 copies back-to-back, wait for all
              - Tests: destroy thread while idle (clean shutdown)

            Output complete av1_copy_thread.h, av1_copy_thread.c, and test_copy_thread.c.
        """),
        "checklist": [
            "Byte-identical copy (memcmp src vs dst)",
            "Multiple sequential copies work",
            "Clean shutdown (no thread leak, no hang)",
            "ThreadSanitizer clean",
            "Atomic status transitions are correct",
        ],
    },
    # -----------------------------------------------------------------------
    # Prompt 5
    # -----------------------------------------------------------------------
    {
        "id": 5,
        "name": "av1_decode",
        "title": "av1_decode — Serial Decode with AU = TD-to-TD",
        "depends_on": [1, 2, 3],
        "prompt": textwrap.dedent("""\
            Implement av1_decode() for the AV1 decoder.

            You have already implemented (provided as context above):
            - Prompt 1: av1_mem_override (memory allocator)
            - Prompt 2: av1_job_queue (frame queue)
            - Prompt 3: av1_decoder_api.h/.c (av1_query_memory, av1_create_decoder)

            Key constraints:
            - The pipeline is SERIAL — decode one frame at a time, fully, before
              accepting the next DECODE call. (Async decode is a future optimization.)
            - An AU is all OBUs from one Temporal Delimiter to the next.
            - If the ready queue is full (queue_depth frames waiting for pickup),
              return AV1_QUEUE_FULL.
            - On success, the decoded frame is placed in the ready queue for av1_sync().

            Add to: av1_decoder_api.c

            av1_decode(decoder, data, data_size, out_result):
              1. If decoder state != CREATED and != DECODING -> return INVALID_PARAM
              2. Set state = DECODING
              3. If ready queue is full -> return AV1_QUEUE_FULL
              4. Call the existing AOM decode path:
                 - aom_codec_decode() internally, which calls
                   aom_decode_frame_from_obus() -> av1_decode_frame_headers_and_setup()
                   -> av1_decode_tg_tiles_and_wrapup()
                 - Runs SYNCHRONOUSLY on the caller's thread.
              5. After decode completes:
                 - If show_frame or show_existing_frame:
                   a. Assign a frame_id (monotonic counter)
                   b. Push {frame_id, dpb_slot, show_frame, show_existing} to ready queue
                   c. Fill out_result: frame_ready=1, frame_id, show_existing_frame flag
                 - If !show_frame: out_result.frame_ready=0
              6. Return AV1_OK

            Integration with AOM:
              - The Av1Decoder wraps an aom_codec_ctx_t initialized in av1_create_decoder.
              - av1_decode() calls aom_codec_decode() on that internal context.
              - After decode, check aom_codec_get_frame() for output.
              - show_existing_frame is handled by AOM internally — just check for output.

            Also provide a minimal IVF file parser for the test:
              - 32 byte header: "DKIF", version(2), header_size(2), fourcc(4),
                width(2), height(2), timebase_num(4), timebase_den(4), num_frames(4), unused(4)
              - Per frame: size(4 LE) + timestamp(8 LE) + data[size]

            Write a test (test_av1_decode.c) that:
              - Creates decoder via the API
              - Reads a .ivf file
              - Calls av1_decode() for each frame
              - Checks out_result.frame_ready
              - Decodes queue_depth+1 frames without draining -> verify QUEUE_FULL
              - Prints frame_id and status for each decoded frame

            Output the updated av1_decoder_api.h, av1_decoder_api.c (with av1_decode added),
            ivf_parser.h, ivf_parser.c, and test_av1_decode.c.
        """),
        "checklist": [
            "Decodes AV1 bitstream without crashing",
            "Frame ready flag set correctly for show_frame / show_existing",
            "Non-display frames (reference only) produce frame_ready=0",
            "QUEUE_FULL returned when ready queue is full",
            "Frame IDs are monotonically increasing",
            "State transitions correct (CREATED -> DECODING)",
        ],
    },
    # -----------------------------------------------------------------------
    # Prompt 6
    # -----------------------------------------------------------------------
    {
        "id": 6,
        "name": "sync_output",
        "title": "av1_sync, av1_set_output, av1_receive_output",
        "depends_on": [2, 3, 4, 5],
        "prompt": textwrap.dedent("""\
            Implement the output retrieval pipeline for the AV1 decoder.

            You have already implemented (provided as context above):
            - av1_job_queue (frame queue)
            - av1_decoder_api (query, create, decode)
            - av1_copy_thread (copy worker)

            Add to: av1_decoder_api.c

            av1_sync(decoder, timeout_us, out_result):
              1. Pop from ready queue (using timeout from the queue)
              2. If got a frame: fill out_result, return AV1_OK
              3. If timeout and no frame: return AV1_NEED_MORE_DATA
              4. If state==FLUSHING and ready queue empty: return AV1_END_OF_STREAM

            av1_set_output(decoder, frame_id, output_buffer):
              1. Look up the frame by frame_id in a "pending output" table
                 (av1_sync moves entries from ready_queue to this table)
              2. Get the DPB slot's YV12_BUFFER_CONFIG (source planes, strides)
              3. Build an Av1CopyJob from source + output_buffer destination
              4. Enqueue to copy thread
              5. Return AV1_OK

            av1_receive_output(decoder, frame_id, timeout_us):
              1. Wait for the copy job with this frame_id to reach status COMPLETE
              2. Release the DPB reference (ref_count--)
              3. Return AV1_OK (or timeout error)

            Data flow:
              av1_decode -> ready_queue -> av1_sync -> pending_output -> av1_set_output
              -> copy_thread -> av1_receive_output -> done, DPB slot freed

            The "pending output" is a small array (queue_depth entries) mapping
            frame_id -> dpb_slot + copy_job.

            Write a test (test_sync_output.c) — end-to-end:
              - Create decoder, read .ivf file, decode each frame
              - For each frame_ready: sync -> set_output -> receive_output -> write .y4m
              - Compare output against aomdec reference (bit-exact)

            Also write a y4m_writer.h/.c helper for writing Y4M output.

            Output updated av1_decoder_api.h/.c, y4m_writer.h/.c, and test_sync_output.c.
        """),
        "checklist": [
            "Full decode -> sync -> set_output -> receive_output pipeline works",
            "Output is bit-exact with aomdec reference output",
            "Multiple frames decoded and output correctly",
            "DPB slots are freed after receive_output (no leak / exhaustion)",
            "Timeout works on av1_sync and av1_receive_output",
        ],
    },
    # -----------------------------------------------------------------------
    # Prompt 7
    # -----------------------------------------------------------------------
    {
        "id": 7,
        "name": "flush_destroy",
        "title": "av1_flush and av1_destroy_decoder",
        "depends_on": [3, 4, 5, 6],
        "prompt": textwrap.dedent("""\
            Implement the cleanup functions for the AV1 decoder.

            You have already implemented (provided as context above):
            - The full API: query, create, decode, sync, set_output, receive_output
            - Copy thread, job queue, memory override

            Add to: av1_decoder_api.c

            av1_flush(decoder):
              1. Set state = FLUSHING
              2. The serial pipeline means there's no in-flight async work to drain.
                 All decoded frames are already in the ready queue.
              3. Reject any subsequent av1_decode() calls (return error).
              4. The app drains remaining frames via sync -> set_output -> receive_output
                 until av1_sync returns AV1_END_OF_STREAM.
              5. Return AV1_OK

            av1_destroy_decoder(decoder):
              1. If not flushed, flush first (or force-drain)
              2. Wait for any in-progress copy to complete
              3. Signal copy thread to exit, join it
              4. Signal worker threads to exit, join them
              5. If GPU thread exists, signal and join it
              6. Destroy internal AOM codec context (aom_codec_destroy)
              7. Destroy mutexes, condvars
              8. Zero the decoder struct (security wipe)
              9. Clear the "use_pool" flag (restore normal malloc)
              10. Return AV1_OK

            Error states:
              - Destroy while copy in progress -> wait for copy, then destroy
              - Destroy without prior flush -> implicit flush + discard
              - Double destroy -> return error or no-op

            Write a test (test_flush_destroy.c):
              - Normal: decode 10 frames -> flush -> drain all -> destroy -> free memory
              - Early destroy: decode 5 frames -> destroy without flush -> no hang
              - Empty: create -> destroy immediately -> no crash

            Output updated av1_decoder_api.h/.c and test_flush_destroy.c.
        """),
        "checklist": [
            "Flush + drain cycle works (END_OF_STREAM returned correctly)",
            "Destroy after flush is clean",
            "Destroy without flush doesn't hang",
            "Destroy with no decodes doesn't crash",
            "No thread leaks (all threads joined)",
            "AddressSanitizer clean (no leaks)",
        ],
    },
    # -----------------------------------------------------------------------
    # Prompt 8
    # -----------------------------------------------------------------------
    {
        "id": 8,
        "name": "gpu_thread_stub",
        "title": "GPU Thread Stub",
        "depends_on": [2, 4],
        "prompt": textwrap.dedent("""\
            Implement a GPU thread STUB for the AV1 decoder.

            You have already implemented (provided as context above):
            - av1_job_queue (for reference on queue pattern)
            - av1_copy_thread (for reference on thread lifecycle)

            This is NOT a GPU implementation. It's the thread lifecycle and data flow
            infrastructure that a real GPU implementation would plug into.

            The GPU thread's purpose: free the CPU from reconstruction + filtering work
            so the CPU is available for game logic.

            Produce: av1_gpu_thread.h and av1_gpu_thread.c

            The GPU thread:
              1. Receives a "GPU job" after CPU entropy decode completes
              2. In the stub: marks the job as complete after a simulated delay
              3. Real impl would: build command buffers, submit to GPU, wait on fence

            For the FILM GRAIN path (document in comments, don't implement):
              - GPU compute shader reads un-grained frame from DPB (GPU memory)
              - Synthesizes film grain per AV1 spec
              - Converts pixel format (planar YUV -> NV12, 10-bit packed, etc.)
                based on a "dst texture descriptor"
              - Writes directly to the destination buffer from SET OUTPUT
              - Copy thread is NOT used in GPU mode — the grain shader IS the copy

            Structures:
            typedef struct Av1GpuJob {
                uint32_t frame_id;
                int      dpb_slot;
                int      needs_film_grain;
                void    *dst_descriptor;     // opaque — GPU API specific
                _Atomic int status;          // PENDING=0, PROCESSING=1, COMPLETE=2
            } Av1GpuJob;

            typedef struct Av1GpuThread {
                pthread_t thread;
                pthread_mutex_t mutex;
                pthread_cond_t  cond;
                pthread_cond_t  done_cond;
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
                // GPU_IMPL: upload symbol data here
                // GPU_IMPL: build inverse transform compute dispatch here
                // GPU_IMPL: build intra/inter prediction dispatch here
                // GPU_IMPL: build loop filter + CDEF + LR dispatch here
                // GPU_IMPL: build film grain + format convert + output dispatch here
                // GPU_IMPL: submit command buffer and wait on fence here
                usleep(1000);  // STUB: simulate 1ms of GPU processing
                set status = COMPLETE
                signal done_cond
              }

            Write a test (test_gpu_thread.c):
              - Create GPU thread stub
              - Enqueue 10 fake jobs
              - Wait for each to complete
              - Verify all complete in order
              - Destroy cleanly

            Output complete av1_gpu_thread.h, av1_gpu_thread.c, and test_gpu_thread.c.
        """),
        "checklist": [
            "Thread starts and stops cleanly",
            "Jobs complete in FIFO order",
            "Stub delay works (each job takes ~1ms)",
            "Clean shutdown with in-flight jobs",
            "Comments clearly mark all GPU_IMPL extension points",
            "Film grain + format conversion + direct-to-dst path is documented",
        ],
    },
    # -----------------------------------------------------------------------
    # Prompt 9
    # -----------------------------------------------------------------------
    {
        "id": 9,
        "name": "integration_test",
        "title": "End-to-End Integration Test",
        "depends_on": [1, 2, 3, 4, 5, 6, 7],
        "prompt": textwrap.dedent("""\
            Write a complete end-to-end test program for the AV1 decoder API.

            You have the complete API implemented (provided as context above):
            - av1_mem_override (memory), av1_job_queue (queue), av1_copy_thread (copy),
              av1_gpu_thread (GPU stub), av1_decoder_api (full API), ivf_parser, y4m_writer

            This program:
              1. Opens an .ivf file
              2. Parses the IVF header to get width/height
              3. Parses the first frame to extract the AV1 Sequence Header OBU
                 (to get bit_depth, chroma subsampling, etc.)
              4. Calls av1_query_memory() with stream info
              5. Allocates the memory block
              6. Calls av1_create_decoder()
              7. Decode loop:
                 - Read next AU from IVF
                 - Call av1_decode()
                 - If QUEUE_FULL: drain via sync -> set_output -> receive_output, retry
                 - If frame_ready: sync -> set_output -> receive_output -> write to .y4m
              8. av1_flush()
              9. Drain remaining: sync -> set_output -> receive_output until END_OF_STREAM
              10. av1_destroy_decoder()
              11. Free memory

            Also provide:
              - A Makefile that compiles everything together against libaom
              - A run_conformance.sh script that:
                a. Decodes a test vector with aomdec to reference.y4m
                b. Decodes with our test program to test.y4m
                c. Runs: diff reference.y4m test.y4m (must be identical)

            Test error paths too:
              - Truncated bitstream (fewer bytes than IVF says)
              - Zero-length AU
              - QUEUE_FULL recovery

            Output: test_e2e.c, Makefile, run_conformance.sh
        """),
        "checklist": [
            "Successfully decodes at least one AV1 .ivf test vector",
            "Output matches aomdec bit-exactly",
            "QUEUE_FULL handling works (drain + retry succeeds)",
            "Flush drains all remaining frames",
            "No memory leaks (AddressSanitizer clean)",
            "No thread leaks (all threads joined at exit)",
            "IVF parser handles edge cases (empty file, truncated frame)",
        ],
    },
]


# ---------------------------------------------------------------------------
# Helper: send a prompt to the vLLM OpenAI-compatible endpoint
# ---------------------------------------------------------------------------

def send_prompt_streaming(base_url: str, model: str, system: str, messages: list,
                          max_tokens: int, temperature: float, timeout: int,
                          log_fn=None) -> dict:
    """Send a chat completion request to vLLM with streaming enabled.

    Streams the response so partial output is captured even on timeout.
    Returns a dict matching the non-streaming response format:
      {"choices": [{"message": {"content": text}, "finish_reason": reason}],
       "usage": {...}}
    """
    url = f"{base_url.rstrip('/')}/chat/completions"
    payload = {
        "model": model,
        "messages": [{"role": "system", "content": system}] + messages,
        "max_tokens": max_tokens,
        "temperature": temperature,
        "stream": True,
        "stream_options": {"include_usage": True},
        "tool_choice": "none",
    }

    collected_text = []
    finish_reason = None
    usage = {}
    timed_out = False

    try:
        resp = requests.post(url, json=payload, timeout=(30, timeout), stream=True)
        resp.raise_for_status()

        for line in resp.iter_lines(decode_unicode=True):
            if not line or not line.startswith("data: "):
                continue
            data_str = line[len("data: "):]
            if data_str.strip() == "[DONE]":
                break
            try:
                chunk = json.loads(data_str)
            except json.JSONDecodeError:
                continue

            # Usage comes in the final chunk (stream_options.include_usage)
            if chunk.get("usage"):
                usage = chunk["usage"]

            choices = chunk.get("choices", [])
            if not choices:
                continue

            delta = choices[0].get("delta", {})
            if "content" in delta and delta["content"]:
                collected_text.append(delta["content"])

            if choices[0].get("finish_reason"):
                finish_reason = choices[0]["finish_reason"]

    except requests.exceptions.ReadTimeout:
        timed_out = True
        if log_fn:
            log_fn(f"  ⏱ Stream timed out — captured {len(collected_text)} chunks so far")
    except requests.exceptions.ConnectionError:
        if collected_text:
            timed_out = True
            if log_fn:
                log_fn(f"  ⏱ Connection lost — captured {len(collected_text)} chunks")
        else:
            raise

    text = "".join(collected_text)

    if timed_out and not finish_reason:
        finish_reason = "timeout"

    return {
        "choices": [{"message": {"content": text}, "finish_reason": finish_reason or "unknown"}],
        "usage": usage,
    }


# ---------------------------------------------------------------------------
# Helper: extract code blocks from model output and save as files
# ---------------------------------------------------------------------------

def extract_and_save_files(text: str, prompt_dir: Path):
    """
    Try to extract ```c or ```h fenced code blocks with filenames.
    Also save the full raw response.
    """
    prompt_dir.mkdir(parents=True, exist_ok=True)

    # Save raw response
    (prompt_dir / "raw_response.md").write_text(text, encoding="utf-8")

    # Try to extract files from fenced blocks
    import re
    # Match patterns like: ```c\n// file: av1_mem_override.h  OR  **av1_mem_override.h**\n```c
    blocks = re.findall(
        r'(?:(?:^|\n)\s*(?:\*\*|`)?(\w[\w./]*\.[ch])\**`?\s*\n)?'
        r'```[ch]?\n(.*?)```',
        text, re.DOTALL
    )

    # Also try: lines like "### av1_mem_override.h" or "// File: av1_mem_override.h"
    # followed by a code block
    named_blocks = re.findall(
        r'(?:#{1,4}\s+|//\s*[Ff]ile:\s*|`)`?(\w[\w./]*\.[ch])`?\s*\n+```[ch]?\n(.*?)```',
        text, re.DOTALL
    )
    blocks.extend(named_blocks)

    saved = []
    unnamed_idx = 0
    for filename, code in blocks:
        if not filename or filename.strip() == "":
            unnamed_idx += 1
            filename = f"unnamed_{unnamed_idx}.c"
        filename = Path(filename).name  # strip any path prefix
        filepath = prompt_dir / filename
        filepath.write_text(code, encoding="utf-8")
        saved.append(filename)

    return saved


# ---------------------------------------------------------------------------
# Main runner
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Run AV1 decoder prompts on local vLLM")
    parser.add_argument("--base-url", default="http://192.168.1.120:8000/v1",
                        help="OpenAI-compatible API base URL. Works with vLLM and llama.cpp server. "
                             "(default: http://192.168.1.120:8000/v1)")
    parser.add_argument("--model", default=MODEL_NAME,
                        help=f"Model name for the API (default: {MODEL_NAME})")
    parser.add_argument("--max-tokens", type=int, default=MAX_TOKENS,
                        help=f"Max output tokens (default: {MAX_TOKENS})")
    parser.add_argument("--temperature", type=float, default=TEMPERATURE,
                        help=f"Sampling temperature (default: {TEMPERATURE})")
    parser.add_argument("--timeout", type=int, default=REQUEST_TIMEOUT,
                        help=f"Request timeout in seconds (default: {REQUEST_TIMEOUT})")
    parser.add_argument("--start-from", type=int, default=1,
                        help="Start from prompt N (skip earlier ones, use saved outputs as context)")
    parser.add_argument("--only", type=int, default=None,
                        help="Run ONLY prompt N")
    parser.add_argument("--dry-run", action="store_true",
                        help="Print prompts without sending to the model")
    parser.add_argument("--output-dir", type=str, default=str(OUTPUT_DIR),
                        help=f"Output directory (default: {OUTPUT_DIR})")
    parser.add_argument("--rerun-all", action="store_true",
                        help="Re-run all prompts even if they completed successfully before")
    parser.add_argument("--aom-dir", type=str, default=None,
                        help="Path to AOM source tree (auto-detected if not set)")
    args = parser.parse_args()

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    # Determine which prompts to run
    if args.only:
        to_run = [p for p in PROMPTS if p["id"] == args.only]
    else:
        to_run = [p for p in PROMPTS if p["id"] >= args.start_from]

    if not to_run:
        print("ERROR: No prompts to run.")
        sys.exit(1)

    # Load any previously saved outputs for context injection
    saved_outputs = {}
    completed_prompts = set()
    for p in PROMPTS:
        prompt_dir = output_dir / f"prompt_{p['id']:02d}_{p['name']}"
        raw_path = prompt_dir / "raw_response.md"
        meta_path = prompt_dir / "metadata.json"
        if raw_path.exists():
            saved_outputs[p["id"]] = raw_path.read_text(encoding="utf-8")
        # Check if this prompt completed successfully
        if meta_path.exists():
            try:
                meta = json.loads(meta_path.read_text(encoding="utf-8"))
                fr = meta.get("finish_reason", "")
                if fr == "stop":
                    completed_prompts.add(p["id"])
            except (json.JSONDecodeError, KeyError):
                pass

    # Skip already-completed prompts unless --rerun-all
    if not args.rerun_all and completed_prompts:
        before = len(to_run)
        to_run = [p for p in to_run if p["id"] not in completed_prompts]
        skipped = before - len(to_run)
        if skipped > 0:
            print(f"Skipping {skipped} already-completed prompt(s): "
                  f"{sorted(completed_prompts)}. Use --rerun-all to force.")
    if not to_run:
        print("All prompts already completed. Use --rerun-all to re-run.")
        sys.exit(0)

    log_path = output_dir / "run_log.txt"
    def log(msg):
        ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        line = f"[{ts}] {msg}"
        print(line)
        with open(log_path, "a", encoding="utf-8") as f:
            f.write(line + "\n")

    # Find AOM source directory
    aom_base = find_aom_base()
    if args.aom_dir:
        aom_base = Path(args.aom_dir)

    log(f"=== AV1 Decoder Prompt Runner ===")
    log(f"Base URL: {args.base_url}")
    log(f"Model: {args.model}")
    log(f"Max tokens: {args.max_tokens}")
    log(f"Temperature: {args.temperature}")
    log(f"Output dir: {output_dir}")
    log(f"AOM source dir: {aom_base or 'NOT FOUND — prompts will lack source context'}")
    log(f"Running prompts: {[p['id'] for p in to_run]}")
    log(f"Previously completed (will use as context): {sorted(completed_prompts) or 'none'}")
    log(f"Previously saved outputs available: {list(saved_outputs.keys())}")
    log("")

    # Check connectivity (unless dry run)
    if not args.dry_run:
        try:
            r = requests.get(f"{args.base_url.rstrip('/')}/models", timeout=10)
            r.raise_for_status()
            models = r.json()
            log(f"Connected to vLLM. Available models: {json.dumps(models.get('data', []), indent=2)}")
        except Exception as e:
            log(f"WARNING: Could not connect to vLLM at {args.base_url}: {e}")
            log("Continuing anyway — will fail on first prompt if unreachable.")
        log("")

    # Run each prompt
    for prompt_info in to_run:
        pid = prompt_info["id"]
        name = prompt_info["name"]
        title = prompt_info["title"]
        prompt_dir = output_dir / f"prompt_{pid:02d}_{name}"

        log(f"{'='*60}")
        log(f"PROMPT {pid}: {title}")
        log(f"{'='*60}")

        # Build messages: AOM source context + prior outputs + prompt
        messages = []

        # 1. Inject AOM source files as first context message
        aom_context = load_aom_context(pid, aom_base)
        if aom_context:
            messages.append({
                "role": "user",
                "content": aom_context
            })
            messages.append({
                "role": "assistant",
                "content": "I've reviewed the AOM reference decoder source files. I can see the "
                           "struct definitions, memory allocation patterns, decoder API, and "
                           "internal implementation. I'll use these as reference."
            })
            log(f"  Injected AOM source context: {len(aom_context)} chars")

        # 2. Inject prior prompt outputs as context
        for dep_id in prompt_info["depends_on"]:
            if dep_id in saved_outputs:
                dep_name = next(p["name"] for p in PROMPTS if p["id"] == dep_id)
                dep_title = next(p["title"] for p in PROMPTS if p["id"] == dep_id)
                dep_text = saved_outputs[dep_id]
                if len(dep_text) > 80000:
                    dep_text = dep_text[:80000] + "\n\n... [TRUNCATED — see full output in saved file] ..."
                messages.append({
                    "role": "user",
                    "content": f"Here is the output from Prompt {dep_id} ({dep_title}) "
                               f"which you previously implemented:\n\n{dep_text}"
                })
                messages.append({
                    "role": "assistant",
                    "content": f"Understood. I have the implementation from Prompt {dep_id} ({dep_title}) as context."
                })
                log(f"  Injecting prior output: Prompt {dep_id} ({dep_name}) — {len(dep_text)} chars")
            else:
                log(f"  WARNING: Dependency Prompt {dep_id} not found in saved outputs!")

        # 3. Add the actual prompt with reinforcement
        prompt_text = prompt_info["prompt"] + "\n\n" + (
            "IMPORTANT: Output the complete file contents directly in your response. "
            "Use markdown fenced code blocks with a ### filename heading before each file. "
            "Do NOT attempt to read files, call tools, or output XML. "
            "Generate all code from scratch based on the requirements and AOM source above."
        )
        messages.append({"role": "user", "content": prompt_text})

        if args.dry_run:
            total_chars = sum(len(m["content"]) for m in messages)
            est_tokens = total_chars // 3  # rough estimate: ~3 chars/token
            log(f"  [DRY RUN] Would send {len(messages)} messages")
            log(f"  Total input chars: {total_chars:,} (~{est_tokens:,} tokens estimated)")
            log(f"  Prompt text: {len(prompt_info['prompt'])} chars")
            log(f"  AOM context: {len(aom_context)} chars")
            log(f"  Budget remaining: ~{131000 - est_tokens - args.max_tokens:,} tokens")
            log("")
            continue

        # Send to model with continuation support
        # If the model truncates (finish_reason=length) or times out with partial
        # output, we save the checkpoint and ask it to continue.
        start_time = time.time()
        full_text = ""
        continuation = 0
        cur_messages = list(messages)
        total_usage = {"prompt_tokens": 0, "completion_tokens": 0, "total_tokens": 0}
        final_finish_reason = None

        try:
            while continuation <= MAX_CONTINUATIONS:
                label = f"(continuation {continuation})" if continuation > 0 else ""
                log(f"  Sending to model ({len(cur_messages)} messages) {label}...")

                response = send_prompt_streaming(
                    base_url=args.base_url,
                    model=args.model,
                    system=SYSTEM_PROMPT,
                    messages=cur_messages,
                    max_tokens=args.max_tokens,
                    temperature=args.temperature,
                    timeout=args.timeout,
                    log_fn=log,
                )
                elapsed = time.time() - start_time

                choice = response["choices"][0]
                text = choice["message"]["content"]
                finish_reason = choice.get("finish_reason", "unknown")
                usage = response.get("usage", {})

                # Accumulate usage
                for k in total_usage:
                    total_usage[k] += usage.get(k, 0)

                log(f"  Received response in {elapsed:.1f}s total")
                log(f"  Finish reason: {finish_reason}")
                log(f"  Tokens — prompt: {usage.get('prompt_tokens', '?')}, "
                    f"completion: {usage.get('completion_tokens', '?')}, "
                    f"total: {usage.get('total_tokens', '?')}")

                full_text += text

                # Check if we need to continue
                if finish_reason in ("length", "timeout") and text.strip():
                    continuation += 1
                    if continuation > MAX_CONTINUATIONS:
                        log(f"  ⚠ Hit max continuations ({MAX_CONTINUATIONS}), saving partial output")
                        final_finish_reason = f"{finish_reason}_max_continuations"
                        break

                    # Save checkpoint
                    checkpoint_path = prompt_dir / f"checkpoint_{continuation}.md"
                    prompt_dir.mkdir(parents=True, exist_ok=True)
                    checkpoint_path.write_text(full_text, encoding="utf-8")
                    log(f"  ⚠ Output incomplete ({finish_reason}), saved checkpoint ({len(full_text)} chars)")
                    log(f"  → Sending continuation {continuation}/{MAX_CONTINUATIONS}...")

                    # Build continuation: append what the model wrote so far,
                    # then ask it to continue from where it stopped
                    cur_messages = list(messages)  # start from original context
                    cur_messages.append({
                        "role": "assistant",
                        "content": full_text
                    })
                    cur_messages.append({
                        "role": "user",
                        "content": (
                            "Your previous response was cut off. Continue EXACTLY where you "
                            "left off — do not repeat any code you already wrote. "
                            "Pick up from the last line and keep going until all files are complete."
                        )
                    })
                elif finish_reason == "timeout" and not text.strip():
                    # Timed out with no output at all
                    log(f"  ✗ TIMEOUT with no output after {elapsed:.1f}s (limit: {args.timeout}s)")
                    log(f"  Skipping to next prompt. Re-run with: --only {pid} --timeout {args.timeout * 2}")
                    final_finish_reason = "timeout_empty"
                    break
                else:
                    # Normal completion
                    final_finish_reason = finish_reason
                    break

            if final_finish_reason == "timeout_empty":
                continue

            # Save outputs
            saved_files = extract_and_save_files(full_text, prompt_dir)
            log(f"  Saved raw response + extracted files: {saved_files}")
            if continuation > 0:
                log(f"  Total output: {len(full_text)} chars across {continuation + 1} requests")

            # Save the response for use as context by later prompts
            saved_outputs[pid] = full_text

            # Save metadata
            meta = {
                "prompt_id": pid,
                "prompt_name": name,
                "timestamp": datetime.now().isoformat(),
                "elapsed_seconds": time.time() - start_time,
                "finish_reason": final_finish_reason,
                "usage": total_usage,
                "continuations": continuation,
                "model": args.model,
                "temperature": args.temperature,
                "max_tokens": args.max_tokens,
                "extracted_files": saved_files,
                "checklist": prompt_info["checklist"],
                "response_hash": hashlib.sha256(full_text.encode()).hexdigest()[:16],
            }
            (prompt_dir / "metadata.json").write_text(
                json.dumps(meta, indent=2), encoding="utf-8"
            )

            # Print checklist reminder
            log(f"  Verification checklist for Prompt {pid}:")
            for item in prompt_info["checklist"]:
                log(f"    [ ] {item}")

        except requests.exceptions.ConnectionError as e:
            log(f"  ✗ CONNECTION ERROR: {e}")
            log(f"  Is vLLM running at {args.base_url}?")
            if full_text.strip():
                log(f"  Saving {len(full_text)} chars of partial output before exit")
                prompt_dir.mkdir(parents=True, exist_ok=True)
                extract_and_save_files(full_text, prompt_dir)
            sys.exit(1)
        except requests.exceptions.HTTPError as e:
            log(f"  ✗ HTTP ERROR: {e}")
            try:
                log(f"  Response body: {e.response.text[:500]}")
            except Exception:
                pass
            continue
        except Exception as e:
            log(f"  ✗ UNEXPECTED ERROR: {type(e).__name__}: {e}")
            if full_text.strip():
                log(f"  Saving {len(full_text)} chars of partial output")
                prompt_dir.mkdir(parents=True, exist_ok=True)
                extract_and_save_files(full_text, prompt_dir)
            continue

        log("")

    log("=== Run complete ===")
    log(f"Outputs saved to: {output_dir}")
    log(f"Log saved to: {log_path}")


if __name__ == "__main__":
    main()
