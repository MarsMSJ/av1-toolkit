# Design Decisions & Constraints

Captured 2026-04-09. These override any conflicting assumptions in the other design docs.

## Pipeline Model

1. **Queue depth is serial, not parallel.** Frames are decoded one at a time in order. Queue depth controls how many decoded frames can sit waiting for SET OUTPUT / RECEIVE OUTPUT before backpressure kicks in. There is no multi-frame parallel decode — no dependency deadlock problem exists.

2. **AU definition**: An Access Unit is everything from one Temporal Delimiter OBU to the next. All OBUs between two TDs belong to a single AU. The DECODE API receives one complete AU at a time. No partial-frame handling needed.

3. **200µs DECODE budget is a future optimization target**, not a correctness requirement. The AOM reference decoder is for accuracy, not speed. The initial implementation can be synchronous inside DECODE and we refactor for async later.

## Memory

4. **malloc override is acceptable.** We can override aom_malloc/aom_memalign/aom_free to route through the pool allocator without rewriting every call site. No need to build a custom allocator from scratch initially — just redirect the existing allocation functions.

## GPU Thread

5. **GPU thread purpose**: Free the CPU for game developer work. The CPU should not be doing heavy reconstruction/filtering. The GPU thread is about offloading, not about raw decode speed.

6. **Film grain as GPU shader**: Film grain synthesis will be a GPU compute shader that writes directly into the destination buffer provided by SET OUTPUT. This shader supports format conversion (e.g., dst texture descriptor for the target surface format). This means:
   - The copy thread for GPU mode becomes a "grain + copy" shader dispatch
   - The DPB stays un-grained (correct for references)
   - Format conversion (e.g., NV12, 10-bit packed) happens in the same pass
   - Zero extra CPU-side copy for the grain path

## What this simplifies

| Original concern | Status |
|---|---|
| Frame-to-frame dependency deadlocks | **Eliminated** — serial decode, no parallel frame deps |
| 200µs budget | **Deferred** — optimize later, sync decode is fine for now |
| Multi-tile-group partial AUs | **Eliminated** — AU is TD-to-TD, always complete |
| Custom pool allocator (Phase 0) | **Simplified** — override malloc, done |
| GPU intra wavefront | **Deferred** — not implementing GPU recon now |
| Film grain + copy interaction | **Solved** — FG is a GPU shader writing to dst directly |
