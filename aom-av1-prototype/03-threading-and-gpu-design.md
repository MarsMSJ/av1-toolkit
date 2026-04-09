# Threading Architecture & GPU Thread Design

## Thread Model Overview

```mermaid
flowchart TB
    subgraph "Thread 0: Application / Caller"
        direction TB
        API["av1_decode()<br/>av1_sync()<br/>av1_set_output()<br/>av1_receive_output()"]
    end

    subgraph "Threads 1..N: Worker Pool"
        direction TB
        W["Tile/Row Workers<br/>- Entropy (CABAC) decode<br/>- Inverse transform<br/>- Intra/Inter prediction<br/>- Loop Filter rows<br/>- CDEF rows<br/>- Loop Restoration rows<br/>- Film Grain synthesis"]
    end

    subgraph "Thread N+1: Copy Thread"
        direction TB
        CT["Copy Worker<br/>- DMA / memcpy frame planes<br/>- Signal completion events<br/>- Release DPB slots"]
    end

    subgraph "Thread N+2: GPU Thread (optional)"
        direction TB
        GT["GPU Worker<br/>- Build command buffers<br/>- Submit GPU work<br/>- Fence sync / poll<br/>- Signal frame ready"]
    end

    API -->|"job queue<br/>(lock-free ring)"| W
    W -->|"ready queue"| API
    API -->|"copy queue"| CT
    CT -->|"done event"| API
    API -.->|"GPU job queue<br/>(when gpu=1)"| GT
    GT -.->|"ready queue"| API
```

---

## CPU-Only Decode Pipeline (Detailed)

This is the current AOM model, restructured to be asynchronous.

```mermaid
flowchart LR
    subgraph "DECODE call (< 200µs)"
        P1[Parse OBU headers] --> P2[Parse frame header]
        P2 --> P3[Allocate DPB slot<br/>from pool]
        P3 --> P4[Setup tile info<br/>+ partition bitstream<br/>per tile]
        P4 --> P5[Enqueue to<br/>Work Queue]
    end

    subgraph "Worker Threads (async)"
        P5 --> E1

        subgraph "Per-Tile / Per-Row"
            E1[Entropy decode<br/>read symbols via CDF] --> R1[Inverse quantize<br/>+ inverse transform]
            R1 --> R2[Intra/Inter prediction<br/>+ reconstruction]
        end

        R2 --> LF[Loop Filter<br/>row-parallel]
        LF --> CD[CDEF<br/>row-parallel]
        CD --> LR[Loop Restoration<br/>row-parallel]
        LR --> FG[Film Grain<br/>synthesis]
        FG --> RDY[Mark frame READY<br/>→ Ready Queue]
    end

    subgraph "Copy Thread"
        RDY -.->|av1_set_output| CP[memcpy Y/U/V<br/>to output planes]
        CP --> DONE[Signal<br/>av1_receive_output]
    end
```

### What Stays on the Caller's Thread (the 200µs budget)

The critical constraint: `av1_decode()` must return in **< 200µs**. From profiling AOM, here's what fits:

| Operation | Typical Time (4K) | On caller thread? |
|---|---|---|
| OBU header parsing | ~5µs | Yes |
| Sequence header parse | ~10µs (first AU only) | Yes |
| Frame header parse | ~20–50µs | Yes |
| Tile info + bitstream partitioning | ~10–30µs | Yes |
| **Entropy decode (CABAC)** | **2–15ms** | **No — worker threads** |
| Reconstruction | 5–20ms | No — worker threads |
| Loop filter | 1–5ms | No — worker threads |
| CDEF + LR | 1–3ms | No — worker threads |
| Film grain | 0.5–2ms | No — worker threads |

The dividing line is clear: **header/syntax parsing** happens on the caller thread; everything from entropy decode onward is async.

### show_existing_frame Fast Path

When `show_existing_frame = 1`, the frame header says "output reference frame N directly." No decoding happens. This completes entirely within `av1_decode()`:

```mermaid
flowchart LR
    D[av1_decode] --> H[Parse header:<br/>show_existing = 1]
    H --> REF[Look up ref_frame_map<br/>slot N]
    REF --> INC[ref_count++]
    INC --> RDY[Enqueue to<br/>Ready Queue immediately]
    RDY --> RET[Return AV1_OK<br/>frame_ready = 1]
```

---

## GPU-Assisted Decode Pipeline

### Design Philosophy

The GPU thread replaces **reconstruction + filtering** — not entropy decoding. This is because:

1. **Entropy/CABAC is inherently serial** within a tile (symbol dependencies). It stays on CPU.
2. **Inverse transform, prediction, and filtering are massively parallel** — perfect for GPU compute shaders.
3. The GPU thread receives decoded symbols/coefficients and drives GPU execution.

```mermaid
flowchart TB
    subgraph "DECODE (caller, < 200µs)"
        P[Parse OBU + frame header<br/>+ partition tiles]
    end

    subgraph "CPU Worker Threads"
        E[Entropy decode only<br/>Output: coefficients,<br/>mode info, MVs, etc.]
    end

    subgraph "GPU Thread"
        direction TB
        G1["Receive decoded symbols<br/>(coeffs, modes, MVs per SB)"]
        G1 --> G2["Upload symbol data<br/>to GPU buffers"]
        G2 --> G3["Build command buffer:<br/>1. Inverse quant + IDCT (compute)<br/>2. Intra prediction (compute)<br/>3. Inter prediction (compute + sample)<br/>4. Reconstruct (compute)<br/>5. Loop filter (compute)<br/>6. CDEF (compute)<br/>7. Loop restoration (compute)<br/>8. Film grain (compute)"]
        G3 --> G4["Submit command buffer<br/>to GPU queue"]
        G4 --> G5["Wait on fence /<br/>poll completion"]
        G5 --> G6["Frame in GPU memory<br/>→ Ready Queue"]
    end

    subgraph "Copy Thread"
        C["Copy from GPU-visible mem<br/>to output buffer<br/>(or zero-copy if output is GPU)"]
    end

    P -->|job queue| E
    E -->|"symbol buffer<br/>(per-tile complete)"| G1
    G6 -->|ready| C
```

### Data Flow: CPU → GPU

The CPU entropy decode produces structured data that the GPU needs. This is the **symbol buffer** — the interface between CPU workers and the GPU thread.

```mermaid
block-beta
    columns 1
    block:symbuf["Symbol Buffer (per AU, per tile)"]
        columns 3
        A["Mode Info Array<br/>- partition tree<br/>- prediction mode<br/>- reference frames<br/>- interp filter<br/>per 4×4 block"]
        B["Motion Vectors<br/>- MVs per block<br/>- compound info<br/>- warp params"]
        C["Quantized Coefficients<br/>- dqcoeff per TX block<br/>- eob positions<br/>- TX type/size"]
    end
    block:extra["Additional GPU Inputs"]
        columns 3
        D["Reference Frames<br/>(already in GPU mem<br/>from previous decodes)"]
        E["Filter Parameters<br/>- LF levels per segment<br/>- CDEF strengths<br/>- LR unit params"]
        F["Sequence/Frame Params<br/>- QP, bit depth<br/>- film grain params<br/>- segmentation map"]
    end
```

### GPU Memory Considerations

```
Per-frame GPU buffers:
  - Reconstructed frame:  align(W) × align(H) × bps × planes  (same as CPU)
  - Symbol buffer:        ~(W/4) × (H/4) × sizeof(ModeInfo)   (~50 MB for 4K)
  - Coefficient buffer:   ~(W/4) × (H/4) × max_coeffs         (~100 MB for 4K)
  - Reference frames:     Already in GPU memory from prior decodes

The symbol + coeff buffers are double-buffered (ping-pong) so CPU can fill
one while GPU processes the other.
```

---

## CPU-Only vs GPU Mode: Configuration

```mermaid
flowchart TB
    CFG{use_gpu config}
    CFG -->|"use_gpu = 0<br/>CPU-only (AVX512)"| CPU_PATH
    CFG -->|"use_gpu = 1<br/>GPU-assisted"| GPU_PATH

    subgraph CPU_PATH["CPU-Only Path"]
        direction LR
        C1[Entropy on workers] --> C2[Recon on workers<br/>AVX512 kernels]
        C2 --> C3[Filters on workers<br/>AVX512 kernels]
        C3 --> C4[Film grain on workers]
        C4 --> C5[Frame in system RAM]
    end

    subgraph GPU_PATH["GPU-Assisted Path"]
        direction LR
        G1[Entropy on CPU workers] --> G2[Upload symbols<br/>to GPU]
        G2 --> G3[GPU compute:<br/>recon + filter + grain]
        G3 --> G4[Frame in GPU memory]
    end

    CPU_PATH --> COPY[Copy Thread]
    GPU_PATH --> COPY
```

### What Changes Between Modes

| Component | CPU-Only | GPU-Assisted |
|---|---|---|
| Entropy decode | CPU worker threads | CPU worker threads (same) |
| Reconstruction | CPU workers (AVX512) | GPU compute shaders |
| Loop filter | CPU workers (row-parallel) | GPU compute |
| CDEF | CPU workers | GPU compute |
| Loop restoration | CPU workers | GPU compute |
| Film grain | CPU workers | GPU compute |
| Frame output location | System RAM (DPB pool) | GPU-visible memory |
| Copy thread source | System RAM | GPU-visible → system RAM (or direct) |
| `av1_query_memory` | Accounts for DPB in system RAM | DPB smaller (only ref frames for entropy), GPU mem separate |
| Worker thread role | Full pipeline | Entropy only |
| Additional thread | None | GPU thread (1) |

---

## Thread Affinity & Priority Mapping

```mermaid
flowchart LR
    subgraph "CPU Cores"
        direction TB
        C0["Core 0<br/>(App thread)"]
        C1["Core 1<br/>Worker 0"]
        C2["Core 2<br/>Worker 1"]
        C3["Core 3<br/>Worker 2"]
        C4["Core 4<br/>Worker 3"]
        C5["Core 5<br/>Copy Thread"]
    end

    subgraph "Priority Levels"
        direction TB
        P1["HIGH: Copy Thread<br/>(latency-sensitive,<br/>display deadline)"]
        P2["NORMAL: Worker Threads<br/>(throughput-oriented)"]
        P3["BELOW_NORMAL: GPU Thread<br/>(mostly waiting on fences)"]
    end
```

The copy thread gets high priority because it's on the critical path to display — a late copy means a dropped frame. Worker threads are throughput-bound. The GPU thread mostly sleeps waiting on fences.

---

## Synchronization Primitives

| Primitive | Used For |
|---|---|
| **Lock-free ring buffer** | Decode job queue (caller → workers), ready queue (workers → caller) |
| **Condition variable** | `av1_sync()` wait, `av1_receive_output()` wait |
| **Atomic counter** | Queue depth tracking, ref_count on DPB slots |
| **Per-row mutex + condvar** | Row-MT dependencies (parse row N before decode row N-2) |
| **Fence / event** | GPU thread completion signal |
