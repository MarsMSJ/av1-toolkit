# Prompt: Scrub Diff Report — Remove Proprietary Terms

> **Target model**: Qwen Code Next (16GB GPU) or any capable LLM  
> **Purpose**: Take the raw diff analysis from Prompt 06 and remove all proprietary/custom terminology, replacing it with standard VP9 reference (libvpx) terms only.  
> **Output**: A sanitized report safe for external sharing, design discussions, or use as context for the AV1 port.

---

## Prompt

```
You are a technical editor specializing in video codec documentation.

I am giving you a detailed diff analysis report comparing a PROPRIETARY VP9
decoder implementation against the Google VP9 reference decoder (libvpx).

Your task: SCRUB the report so that it contains ONLY terminology, function
names, struct names, and concepts from the PUBLIC Google VP9 reference
decoder (libvpx). The goal is to describe the ARCHITECTURAL PATTERNS and
DESIGN DECISIONS without revealing any proprietary implementation details.

## Scrubbing rules

### MUST REMOVE (replace with generic/libvpx equivalents):
- All proprietary function names → describe by purpose using libvpx terms
  Example: "hw_dec_submit_au()" → "the non-blocking decode submission function"
- All proprietary struct/type names → describe using libvpx struct equivalents
  Example: "HwDecContext" → "the decoder context (analogous to VP9Decoder / vpx_codec_alg_priv_t)"
- All proprietary file names/paths → "the custom [purpose] module"
  Example: "hw_dec_api.c" → "the custom API layer module"
- All proprietary enum values/error codes → describe generically
  Example: "HW_DEC_QUEUE_FULL" → "a queue-full status code"
- Console/platform-specific names (SDK names, hardware names, OS API names)
  → "the target platform" or "the platform threading API"
- Team names, project names, codenames → remove entirely
- Internal ticket/bug references → remove entirely
- Proprietary memory API names → "the platform memory allocator"
- Proprietary threading API names → "the platform threading primitives"
- GPU-specific API names if present → "the GPU compute API"

### MUST KEEP (these are public knowledge):
- All libvpx function names: vpx_codec_decode, vpx_codec_get_frame,
  vp9_decode_frame, vp9_read_frame_header, vp9_decode_tile,
  vpx_malloc, vpx_memalign, vpx_free, etc.
- All libvpx struct names: VP9Decoder, VP9_COMMON, BufferPool,
  RefCntBuffer, YV12_BUFFER_CONFIG, VP9Worker, TileWorkerData, etc.
- All VP9 spec terms: superblock, transform, intra prediction,
  inter prediction, loop filter, segmentation, reference frame,
  bool decoder / arithmetic coding, tile, etc.
- Standard CS terms: mutex, condvar, atomic, ring buffer, bump allocator,
  DPB, ref count, FIFO, etc.
- Standard API pattern names: QUERY MEMORY, CREATE DECODER, DECODE,
  SYNC, SET OUTPUT, RECEIVE OUTPUT, FLUSH — these are generic industry
  terms used across many decoders (NVDEC, MediaCodec, console SDKs)

### REWRITE STYLE:
- Describe changes as architectural patterns, not specific implementations
- Use passive voice for proprietary actions: "the decode is submitted to a queue"
  NOT "hw_submit_decode() pushes to the HwJobRing"
- Focus on WHAT changed and WHY, not the exact HOW of the proprietary side
- Use phrases like:
  - "the custom implementation replaces X with..."
  - "instead of libvpx's approach of X, the pattern used is..."
  - "the non-blocking constraint requires that..."
  - "to meet the 200µs return budget, the split occurs at..."

### OUTPUT STRUCTURE:
Maintain the same section structure as the input report. For each section:
1. **libvpx reference behavior** — keep as-is (it's all public)
2. **Architectural pattern used** — describe the custom approach using ONLY
   generic/libvpx terms
3. **Design rationale** — keep, but scrub proprietary details from reasoning
4. **Applicability to AV1** — ADD a new subsection noting how this same
   pattern would apply when porting AOM/libaom (this is the whole point —
   we want to reuse these lessons for the AV1 port)

### FINAL CHECKS:
Before outputting the scrubbed report, verify:
- [ ] No proprietary function/struct/type names remain
- [ ] No console/platform SDK names remain
- [ ] No internal project names or ticket numbers remain
- [ ] No file paths from the proprietary codebase remain
- [ ] All code snippets use libvpx names or generic pseudocode only
- [ ] The report still makes technical sense and is useful for the AV1 port
- [ ] The "Applicability to AV1" subsections reference AOM equivalents:
      VP9Decoder → AV1Decoder, VP9_COMMON → AV1_COMMON,
      vp9_decode_frame → aom_decode_frame_from_obus, etc.

## Input report

<PASTE RAW REPORT FROM PROMPT 06 HERE>
```

---

## How to use

1. Run Prompt 06 first to generate the raw diff analysis
2. Copy the full output from Prompt 06
3. Paste it where indicated in this prompt
4. Run through Qwen Code Next (or any model — this is a text transformation task)
5. Review the output for any missed proprietary terms (grep for known proprietary names)
6. The scrubbed report is safe to:
   - Share with collaborators
   - Use as context for the AV1 port prompts (Prompt 05)
   - Include in design documents
   - Discuss publicly

## Why this two-step process?

Generating the analysis and scrubbing it are separate tasks because:
1. **The analysis needs proprietary access** — the model must see the actual custom code
2. **The scrub is a text transform** — it doesn't need code access, just the report
3. **You can review the raw report** before scrubbing to ensure nothing important is lost
4. **The scrub is verifiable** — you can grep the output for known proprietary terms
5. **Separation of concerns** — if the scrub misses something, you re-run just the scrub, not the whole analysis
