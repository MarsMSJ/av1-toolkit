# Prompt: VP9 Custom Implementation Diff Analysis

> **Target model**: Qwen Code Next (16GB GPU)  
> **Purpose**: Generate a detailed change report comparing a custom VP9 decoder port against the Google VP9 reference decoder (libvpx).  
> **IMPORTANT**: The output of this prompt contains proprietary information. Do NOT share it externally. Use Prompt 07 to scrub before sharing.

---

## Prompt

```
You are a senior video codec engineer performing a detailed code review.

I am providing you with two codebases:
1. The Google VP9 reference decoder (libvpx) — located at: <LIBVPX_PATH>
2. A custom VP9 decoder implementation — located at: <CUSTOM_VP9_PATH>

The custom implementation was ported from the reference decoder to target a
console-style decoder API with the following entry points:
- QUERY MEMORY (determine total memory needed from stream parameters)
- CREATE DECODER (initialize from a caller-provided memory block)
- DECODE (non-blocking, submits an Access Unit, must return within 200µs)
- SYNC (signals/polls when a decode task is complete)
- SET OUTPUT (caller provides destination buffer for frame copy)
- RECEIVE OUTPUT (blocks until frame copy is complete)
- FLUSH (drain the pipeline at end-of-stream)

Your task: produce a DETAILED diff analysis organized by functional area.
For EACH area below, describe what changed, why it likely changed, and how
the custom implementation diverges from the reference.

## Areas to analyze

### 1. Public API Layer
- How does the custom API map to libvpx's vpx_codec_decode() / vpx_codec_get_frame()?
- What new API functions were added? What libvpx API functions were removed/replaced?
- How are error codes and status returns handled differently?

### 2. Memory Management
- How was vpx_malloc/vpx_memalign/vpx_free replaced?
- Is there a pool/bump allocator? How is it structured?
- How are frame buffers (DPB) managed differently?
- How is memory queried/calculated upfront?
- What is the frame buffer slot allocation strategy (bitmask, free list, etc.)?

### 3. Threading Architecture
- What threads exist in the custom implementation vs. libvpx's thread pool?
- Is there a dedicated copy thread? How does it work?
- How are thread priorities and affinities configured?
- What synchronization primitives are used (mutexes, condvars, atomics, lock-free queues)?
- How do worker threads differ in responsibility from libvpx's tile workers?

### 4. Decode Pipeline Split (Non-Blocking DECODE)
- Where exactly is the reference decoder's synchronous decode split into
  parse (caller thread) vs. async decode (worker threads)?
- What happens on the caller thread within the 200µs budget?
- What is queued for async execution?
- How are show_existing_frame and other fast paths handled?

### 5. Queue / Pipeline Management
- What queue data structures are used (ring buffer, linked list, etc.)?
- How does queue depth / backpressure (QUEUE FULL) work?
- How are in-flight frames tracked?
- What is the job lifecycle (submitted → parsing → entropy → recon → filter → ready → copy → done)?

### 6. Reference Frame / DPB Changes
- How does reference frame management differ from libvpx's ref_cnt_fb mechanism?
- How are reference counts managed with the async pipeline?
- When are DPB slots released (after copy? after all references drop?)?

### 7. Bitstream Parsing Changes
- Were any changes made to the actual VP9 bitstream parsing (bool decoder, etc.)?
- How is tile data partitioned and distributed to workers?
- Any changes to frame header parsing for the non-blocking constraint?

### 8. Post-Processing / Filtering
- How are loop filter, CDEF-equivalent (VP9 doesn't have CDEF but has loop filter),
  and any other post-processing handled in the async pipeline?
- Are they on worker threads or a separate stage?

### 9. Output / Copy Path
- How does SET OUTPUT / RECEIVE OUTPUT work internally?
- Is the copy done plane-by-plane? Row-by-row?
- How is the copy thread signaled and how does it signal completion?
- How does film grain (if applicable) interact with the copy path?

### 10. Error Handling & Edge Cases
- How are bitstream errors handled differently in the async pipeline?
- What happens on resolution change mid-stream?
- How does FLUSH drain in-flight work?
- What cleanup happens in DESTROY?

## Output format

For each area, provide:
1. **Reference (libvpx)**: Brief description of how the reference handles this
2. **Custom**: Detailed description of the custom approach
3. **Key files changed**: List of files and the nature of changes
4. **Diff summary**: Pseudo-diff showing the conceptual before/after
5. **Design rationale**: Why this change was likely made (based on the console API constraints)

## Additional instructions
- Be exhaustive. Do not skip minor changes.
- If a file was added (not present in reference), describe it fully.
- If a file was deleted, note what it did and why it was removed.
- Pay special attention to anything that looks like a workaround or hack —
  these often indicate hard problems that were encountered during porting.
- Note any areas where the custom implementation appears incomplete or has TODOs.
- Flag any potential bugs or race conditions you spot.
```

---

## How to use

1. Replace `<LIBVPX_PATH>` with the path to the Google VP9 reference (e.g., a clean libvpx checkout)
2. Replace `<CUSTOM_VP9_PATH>` with the path to the proprietary console VP9 decoder
3. Feed to Qwen Code Next with both codebases accessible
4. The output is the **raw report** — contains proprietary names, structures, and implementation details
5. **Do NOT share the raw output** — run it through Prompt 07 (scrub) first
