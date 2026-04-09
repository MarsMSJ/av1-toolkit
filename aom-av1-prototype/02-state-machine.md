# AV1 Decoder State Machine

## Decoder Lifecycle State Machine

This is the top-level state machine for the decoder instance itself.

```mermaid
stateDiagram-v2
    [*] --> Uninitialized

    Uninitialized --> MemoryQueried : av1_query_memory()
    MemoryQueried --> Created : av1_create_decoder()
    Created --> Decoding : av1_decode() [first AU]
    Decoding --> Decoding : av1_decode() [queue not full]
    Decoding --> QueueFull : av1_decode() returns AV1_QUEUE_FULL
    QueueFull --> Decoding : av1_sync() drains a slot
    Decoding --> Flushing : av1_flush()
    QueueFull --> Flushing : av1_flush()
    Flushing --> Flushing : av1_sync() [frames remain]
    Flushing --> Drained : av1_sync() returns AV1_END_OF_STREAM
    Drained --> Created : Reset (ready for new stream)
    Drained --> Destroyed : av1_destroy_decoder()
    Created --> Destroyed : av1_destroy_decoder()
    Destroyed --> [*]
```

## Explanation

| State | Description |
|---|---|
| **Uninitialized** | No decoder exists. App calls `av1_query_memory()` to determine allocation size. |
| **MemoryQueried** | App knows how much memory to allocate. Allocates and calls `av1_create_decoder()`. |
| **Created** | Decoder is initialized. Internal allocator has carved DPB, scratch, context from the memory block. Worker threads + copy thread are created and idle. |
| **Decoding** | Actively processing AUs. `av1_decode()` accepts new AUs, workers reconstruct frames, copy thread services output requests. |
| **QueueFull** | All `queue_depth` slots are occupied. App **must** drain at least one frame via `av1_sync()` → `av1_set_output()` → `av1_receive_output()` before submitting more. |
| **Flushing** | End-of-stream signaled. No new AUs accepted. App drains remaining frames. |
| **Drained** | All frames output. Decoder can be destroyed or reset for a new sequence. |
| **Destroyed** | Threads joined, all internal state invalidated. App may free the memory block. |

---

## AU (Access Unit) Pipeline State Machine

Each AU submitted via `av1_decode()` progresses through these stages independently. Up to `queue_depth` AUs are in-flight simultaneously.

```mermaid
stateDiagram-v2
    [*] --> Submitted

    Submitted --> Parsing : Syntax parse begins (on DECODE thread)
    Parsing --> Parsed : OBU/syntax parse complete

    state parse_fork <<fork>>
    Parsed --> parse_fork

    parse_fork --> ShowExisting : show_existing_frame=1
    parse_fork --> EntropyQueued : show_existing_frame=0

    ShowExisting --> FrameReady : No decode needed, reference frame already in DPB

    EntropyQueued --> EntropyDecoding : Worker thread picks up job
    EntropyDecoding --> ReconQueued : Entropy/CABAC complete for tile/row

    state recon_decision <<choice>>
    ReconQueued --> recon_decision
    recon_decision --> CPURecon : CPU-only mode
    recon_decision --> GPURecon : GPU mode

    CPURecon --> Filtering : Recon complete → LF + CDEF + LR
    GPURecon --> Filtering : GPU recon complete → post-filter

    Filtering --> FilmGrain : Filters applied
    FilmGrain --> FrameReady : Film grain synthesis done (or skipped)

    FrameReady --> CopyQueued : av1_set_output() called
    CopyQueued --> Copying : Copy thread picks up job
    Copying --> Complete : Copy done → av1_receive_output() unblocks

    Complete --> [*] : DPB slot released (if no longer referenced)
```

### Stage Timing Budget

| Stage | Thread | Blocking? | Target Latency |
|---|---|---|---|
| **Parsing** | Caller's thread (DECODE) | **Non-blocking** (< 200µs) | ~50–150µs for typical 4K frame |
| **Entropy** | Worker thread pool | Non-blocking (async) | ~1–5ms per tile |
| **Reconstruction** | Worker thread pool (or GPU) | Non-blocking (async) | ~2–10ms per frame |
| **Filtering** | Worker threads (row-parallel) | Non-blocking (async) | ~1–5ms per frame |
| **Copy** | Copy thread | Blocks on `av1_receive_output()` | ~0.5–2ms for 4K YUV copy |

---

## Internal Queue & Thread Interaction

```mermaid
flowchart TB
    subgraph "Application Thread"
        A1[av1_decode] -->|"parse OBU headers<br/>< 200µs"| Q1[Decode Job Queue]
        A2[av1_sync] -.->|poll/wait| READY[Ready Queue]
        A3[av1_set_output] -->|enqueue copy job| CQ[Copy Job Queue]
        A4[av1_receive_output] -.->|block until done| CDONE[Copy Done Event]
    end

    subgraph "Worker Thread Pool (N threads)"
        Q1 -->|dequeue| W1[Entropy Decode]
        W1 --> W2[Reconstruction / Inverse Transform]
        W2 --> W3[Loop Filter + CDEF + LR]
        W3 --> W4[Film Grain]
        W4 -->|enqueue| READY
    end

    subgraph "Copy Thread (1 thread)"
        CQ -->|dequeue| C1[memcpy planes to output buffer]
        C1 -->|signal| CDONE
        C1 -->|release DPB slot| DPB[(DPB Frame Buffers)]
    end

    subgraph "GPU Thread (optional, 1 thread)"
        direction TB
        Q1 -.->|GPU mode| G1[Build Command Buffers]
        G1 --> G2[Submit to GPU Queue]
        G2 --> G3[GPU Sync / Fence Wait]
        G3 -->|enqueue| READY
    end

    W2 -.->|read/write| DPB
    G2 -.->|read/write| DPB
```

### Queue Depth & Backpressure

```mermaid
sequenceDiagram
    participant App as Application
    participant Dec as av1_decode (parser)
    participant WQ as Work Queue
    participant Workers as Worker Threads
    participant RQ as Ready Queue
    participant Copy as Copy Thread

    Note over WQ: queue_depth = 3 for this example

    App->>Dec: av1_decode(AU #1)
    Dec->>WQ: Enqueue job #1
    Dec-->>App: AV1_OK

    App->>Dec: av1_decode(AU #2)
    Dec->>WQ: Enqueue job #2
    Dec-->>App: AV1_OK

    Workers->>WQ: Dequeue job #1
    Workers->>Workers: Entropy + Recon + Filter

    App->>Dec: av1_decode(AU #3)
    Dec->>WQ: Enqueue job #3
    Dec-->>App: AV1_OK

    App->>Dec: av1_decode(AU #4)
    Dec-->>App: AV1_QUEUE_FULL ❌

    Workers-->>RQ: Job #1 complete → frame ready
    App->>App: av1_sync() → frame #1 ready
    App->>Copy: av1_set_output(frame #1, dest_buf)
    Copy->>Copy: memcpy Y,U,V planes
    App->>App: av1_receive_output(frame #1) → blocks
    Copy-->>App: Copy complete ✓

    Note over WQ: Slot freed, queue has room

    App->>Dec: av1_decode(AU #4)
    Dec->>WQ: Enqueue job #4
    Dec-->>App: AV1_OK ✓
```

---

## DPB (Decoded Picture Buffer) Management

AV1 uses up to **8 reference frame slots** (`REF_FRAMES = 8`). Additionally, there is always **1 frame currently being decoded**. With a queue depth of N, we need enough buffers for:

```
total_frame_buffers = REF_FRAMES + queue_depth + 1 (safety margin)
                    = 8 + N + 1
```

For `queue_depth = 8`: **17 frame buffers** in the DPB pool.

```mermaid
flowchart LR
    subgraph DPB Pool
        F0[Slot 0<br/>ref_count=2]
        F1[Slot 1<br/>ref_count=1]
        F2[Slot 2<br/>ref_count=0<br/>FREE]
        F3[Slot 3<br/>ref_count=1]
        F4["Slot 4<br/>ref_count=1<br/>(decoding)"]
        F5[Slot 5<br/>ref_count=0<br/>FREE]
        F6[Slot 6<br/>ref_count=1]
        F7["Slot 7<br/>ref_count=1<br/>(awaiting copy)"]
    end

    subgraph "ref_frame_map[8]"
        R0[LAST] --> F0
        R1[LAST2] --> F0
        R2[LAST3] --> F1
        R3[GOLDEN] --> F3
        R4[BWDREF] --> F6
        R5[ALTREF2] --> F6
        R6[ALTREF] --> F3
        R7[unused] --> F2
    end
```

### Reference Count Rules

| Event | ref_count change |
|---|---|
| Frame assigned to `ref_frame_map` slot | +1 |
| Frame removed from `ref_frame_map` slot (replaced) | -1 |
| Frame queued for output (show_frame) | +1 |
| Frame copy completed (`av1_receive_output`) | -1 |
| ref_count reaches 0 | Slot returned to free pool |

---

## Memory Layout (from av1_query_memory / av1_create_decoder)

```mermaid
block-beta
    columns 1
    block:header["Av1Decoder Context (aligned 64B)"]
        A["Av1Decoder struct + internal state<br/>~2 KB"]
    end
    block:dpb["DPB Frame Buffers"]
        B["Frame Buffer Pool<br/>(REF_FRAMES + queue_depth + 1) × frame_size<br/>e.g., 17 × (3840×2160×1.5×2) = ~213 MB for 4K 10-bit"]
    end
    block:scratch["Worker Scratch Buffers"]
        C["Per-worker: mc_buf, tmp_conv, OBMC bufs, seg_mask<br/>num_workers × ~2 MB"]
    end
    block:entropy["Entropy Context"]
        D["FRAME_CONTEXT (CDFs) × queue_depth<br/>~150 KB × N"]
    end
    block:tiledata["Tile Data"]
        E["TileDataDec array, CB_BUFFER (coeff storage)<br/>Depends on tile config"]
    end
    block:queues["Internal Queues & Sync"]
        F["Job queues, condition variables, mutexes<br/>~4 KB"]
    end
    block:threadstacks["Thread Stacks (if allocated from block)"]
        G["(num_workers + 1) × stack_size<br/>Optional — may use OS threads"]
    end
```

### Memory Calculation in av1_query_memory

```
frame_size = align64(width) × align64(height) × bytes_per_sample × planes_factor
           where planes_factor = 1.5 (4:2:0), 2.0 (4:2:2), 3.0 (4:4:4)
           and bytes_per_sample = 1 (8-bit) or 2 (10/12-bit)

dpb_count  = REF_FRAMES + queue_depth + 1

dpb_size   = dpb_count × (frame_size + mv_buffer + seg_map + grain_buffer)
scratch    = num_workers × PER_WORKER_SCRATCH
entropy    = queue_depth × sizeof(FRAME_CONTEXT)
context    = sizeof(Av1Decoder_internal) + tile_data + mode_info
queues     = queue + sync structures

total      = dpb_size + scratch + entropy + context + queues + alignment_padding
```
