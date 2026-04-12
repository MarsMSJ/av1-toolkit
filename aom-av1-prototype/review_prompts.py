#!/usr/bin/env python3
"""
Code review and correction runner for the generated AV1 decoder API.
Sends review prompts to MiniMax-M2.5, one file at a time, asking for
bug identification and corrected code output.

Usage:
    python review_prompts.py                          # review all files
    python review_prompts.py --only av1_mem_override  # review one file
    python review_prompts.py --dry-run                # print without sending
"""

import argparse
import json
import hashlib
import sys
import time
import textwrap
from datetime import datetime
from pathlib import Path

try:
    import requests
except ImportError:
    print("ERROR: 'requests' not installed. Run: pip install requests")
    sys.exit(1)

SCRIPT_DIR    = Path(__file__).parent.resolve()
MERGE_DIR     = SCRIPT_DIR / "verification_outputs" / "merge"
OUTPUT_DIR    = SCRIPT_DIR / "review_outputs"
MODEL_NAME    = "MiniMax-M2.5"
MAX_TOKENS    = 16384
TEMPERATURE   = 0.15   # lower than generation — we want precise analysis
REQUEST_TIMEOUT = 3600  # default 1 hour for all reviews

AOM_BASE_DIR_CANDIDATES = [
    SCRIPT_DIR.parent / "aom",                      # sibling to aom-av1-prototype/
    SCRIPT_DIR.parent / "codec-dev-av1" / "aom",   # local checkout
    Path.home() / "dev-tools" / "aom",             # spark: ~/dev-tools/aom
    Path.home() / "dev-tools" / "av1-toolkit" / "aom",
    Path("/workspace/aom"),                          # spark alternative
]

# ---------------------------------------------------------------------------
# System prompt
# ---------------------------------------------------------------------------

SYSTEM_PROMPT = """\
You are a senior C systems programmer performing a code review. You are NOT \
an agent — you have NO tools, NO file access. Output only text.

You will be given C source files generated for an AV1 decoder API wrapper \
around the AOM reference decoder (libaom). Your job is to:

1. Identify real bugs — not style issues. Focus on:
   - Memory errors (buffer overflows, use-after-free, double-free, leaks)
   - Thread safety issues (races, missing locks, wrong memory ordering)
   - AOM API misuse (wrong codec flags, missing init steps, wrong frame iteration)
   - Logic errors (off-by-one, wrong state transitions, missing error paths)
   - Missing includes or undefined symbols that would cause compile failures

2. For each bug: state the file, line number (approximate), the problem, and the fix.

3. Output a CORRECTED version of each file you found bugs in, complete and \
   compilable, using fenced code blocks preceded by ### filename.

Do NOT output tool calls. Do NOT rewrite files that have no bugs. \
Focus on correctness over style.

## Output format — use this exact structure

Start your response with a markdown checklist of the focus areas you were asked \
to review. Mark each as you complete it. This lets the runner track progress \
if your output is interrupted and needs to be resumed.

```
## Progress
- [x] Focus area 1 — checked, N bugs found
- [x] Focus area 2 — checked, no bugs
- [ ] Focus area 3 — not yet reviewed
```

Then for each completed focus area, write:

```
## Focus area N: <title>
### Bug N.1: <short description>
- **File**: filename.c, line ~NNN
- **Problem**: ...
- **Fix**: ...
```

Finally, output corrected files at the end (only files with bugs). \
If you are interrupted mid-response, the checklist above tells the runner \
which focus areas still need review.
"""

# ---------------------------------------------------------------------------
# Files to review, with their dependencies for context
# ---------------------------------------------------------------------------

REVIEW_TARGETS = [
    {
        "name": "av1_mem_override",
        "files": ["av1_mem_override.h", "av1_mem_override.c"],
        "context_files": [],  # standalone
        "aom_context": ["aom_mem/aom_mem.h", "aom_mem/aom_mem.c"],
        "focus": textwrap.dedent("""\
            Pay special attention to:
            - The global g_mem_header copy vs. the in-place header at base pointer.
              The header is copied INTO g_mem_header but bump_ptr/bump_end point
              into the original base — this creates a dangling pointer bug.
            - The size_t header prepended to each allocation: alignment interactions
              where the returned pointer may not satisfy the requested alignment
              after adding sizeof(size_t) offset.
            - av1_mem_query_size uses info->max_bitrate for bit depth — wrong field
              name (should be bit_depth).
            - Thread safety: g_mem_header is a copy but g_mem_header.bump_ptr is
              the actual allocator state — check if operations on g_mem_header are
              consistent after init.
        """),
    },
    {
        "name": "av1_job_queue",
        "files": ["av1_job_queue.h", "av1_job_queue.c"],
        "context_files": [],
        "aom_context": [],
        "focus": textwrap.dedent("""\
            Pay special attention to:
            - pthread_cond_timedwait usage: timeout_us must be converted to
              an absolute timespec (clock_gettime + offset), not a relative one.
            - Head/tail/count consistency under lock: verify no TOCTOU races
              between checking count and modifying head/tail.
            - Destroy: verify condvar broadcast is sent before pthread_join to
              avoid deadlock if a thread is waiting.
        """),
    },
    {
        "name": "av1_copy_thread",
        "files": ["av1_copy_thread.h", "av1_copy_thread.c"],
        "context_files": ["av1_job_queue.h"],
        "aom_context": [],
        "focus": textwrap.dedent("""\
            Pay special attention to:
            - The copy thread is opaque (forward declaration) — verify the .c
              file defines the struct and all functions compile against it.
            - Copy job wait: if using atomic status polling, there must be a
              condvar or futex to avoid busy-wait. If using a condvar, verify
              the broadcast happens after atomic store, not before.
            - Destroy while copy in-flight: must wait for current job to finish
              before joining the thread, otherwise the copy writes to freed memory.
            - plane_widths vs plane_heights: verify these are bytes-per-row and
              rows respectively, not pixels.
        """),
    },
    {
        "name": "av1_gpu_thread",
        "files": ["av1_gpu_thread.h", "av1_gpu_thread.c"],
        "context_files": ["av1_copy_thread.h"],
        "aom_context": [],
        "focus": textwrap.dedent("""\
            Pay special attention to:
            - av1_gpu_thread_create uses malloc for the thread struct — this must
              use av1_mem_malloc if the override is active, or document why it uses
              system malloc.
            - The GPU_IMPL extension points must be clearly commented — verify
              all 5 extension points are present (upload, inverse transform,
              intra/inter pred, filters, film grain+output).
            - Queue destroy: broadcast condvar before pthread_join.
            - Stub delay: usleep(1000) is fine but must NOT hold the mutex while
              sleeping — check the thread loop structure.
        """),
    },
    # av1_decoder_api is split into 4 focused passes, each covering ~300 lines.
    # line_ranges trims what gets sent so each pass stays under ~4K tokens.
    {
        "name": "av1_decoder_api_helpers",
        "files": ["av1_decoder_api.h", "av1_decoder_api.c"],
        "line_ranges": {"av1_decoder_api.c": (1, 376)},
        "context_files": ["av1_mem_override.h", "av1_job_queue.h"],
        "aom_context": [],
        "focus": textwrap.dedent("""\
            Focus on the static helper functions (lines 1–376 of av1_decoder_api.c):

            1. calculate_frame_buffer_size: verify the plane size math is correct
               for both 8-bit (1 byte/sample) and 10/12-bit (2 bytes/sample), and
               that chroma plane sizes account for 4:2:0 subsampling (width/2, height/2).

            2. find_free_dpb_slot / allocate_dpb_slot / release_dpb_slot: check for
               off-by-one in the DPB loop (REF_FRAMES=8). Verify release zeroes the
               ref_count and does NOT free pixel memory (AOM owns that buffer).

            3. find_pending_output / add_pending_output / remove_pending_output:
               MAX_PENDING_OUTPUT=16 is a fixed array. Verify add_pending_output
               returns an error (not UB) when all 16 slots are full.

            4. worker_thread_func / gpu_thread_func: verify these thread loops check
               a shutdown flag under the same mutex they use for the condvar wait,
               avoiding the lost-wakeup race.
        """),
    },
    {
        "name": "av1_decoder_api_create",
        "files": ["av1_decoder_api.h", "av1_decoder_api.c"],
        "line_ranges": {"av1_decoder_api.c": (377, 687)},
        "context_files": ["av1_mem_override.h", "av1_job_queue.h"],
        "aom_context": [],  # AOM headers are ~17K tokens; focus text describes the API
        "focus": textwrap.dedent("""\
            Focus on av1_query_memory and av1_create_decoder (lines 377–687):

            1. av1_create_decoder: AOM requires aom_codec_dec_init_ver() with
               aom_codec_av1_dx() as the interface. Verify the flags argument and
               the AOM_DECODER_ABI_VERSION macro are passed correctly per
               aom/aom_decoder.h.

            2. Thread creation order: copy thread must be started before worker
               threads because workers post to the copy queue. Verify the order.

            3. Error unwind: if thread N of M fails to create, the code must join
               threads 0..N-1 before returning error. Check this path.

            4. Memory layout: av1_query_memory returns alignment requirements.
               Verify av1_create_decoder checks that config->memory_base is
               aligned to at least that value before initialising the pool.
        """),
    },
    {
        "name": "av1_decoder_api_lifecycle",
        "files": ["av1_decoder_api.h", "av1_decoder_api.c"],
        "line_ranges": {"av1_decoder_api.c": (688, 1047)},
        "context_files": ["av1_mem_override.h", "av1_job_queue.h"],
        "aom_context": [],  # AOM headers are ~17K tokens; focus text describes the API
        "focus": textwrap.dedent("""\
            Focus on av1_flush, av1_destroy_decoder, and av1_decode (lines 688–1047):

            1. av1_decode: after aom_codec_decode(), frames are drained with
               aom_codec_get_frame() and an aom_codec_iter_t initialised to NULL.
               Verify the iterator is reset to NULL at the start of each call and
               the loop continues until aom_codec_get_frame returns NULL.

            2. DPB bridge: aom_codec_get_frame returns aom_image_t*. Verify the
               copy from aom_image_t into Av1DPBSlot is correct — plane[0/1/2]
               pointers, stride[0/1/2], w/h, bit_depth, fmt.

            3. av1_destroy_decoder thread join order: workers must be joined before
               the copy thread (workers may post to copy queue during shutdown).
               The AOM codec (aom_codec_destroy) must come after all threads are joined.

            4. av1_flush state machine: verify the decoder state is set correctly
               after flush — READY (or CREATED?) — and that it is consistent with
               what av1_decode checks on entry.
        """),
    },
    {
        "name": "av1_decoder_api_output_pipeline",
        "files": ["av1_decoder_api.h", "av1_decoder_api.c"],
        "line_ranges": {"av1_decoder_api.c": (1048, 1292)},
        "context_files": [
            "av1_copy_thread.h",
            "av1_gpu_thread.h",
            "av1_job_queue.h",
        ],
        "aom_context": [],
        "focus": textwrap.dedent("""\
            Focus on av1_sync, av1_set_output, av1_receive_output (lines 1048–1292):

            1. av1_sync timeout=0: the header doc says 0 means infinite wait, but
               the underlying queue treats 0 as non-blocking. Find the inconsistency
               and fix it (map 0→UINT32_MAX before passing to the queue, or add a
               special-case loop).

            2. av1_set_output: verify the copy job is built correctly — plane
               pointers and strides from the DPB slot, correct byte widths for
               8-bit (stride bytes) vs 10/12-bit (stride * 2 bytes per row).

            3. av1_receive_output: verify the DPB slot ref_count is decremented
               AFTER the copy job completes, not when it is queued. Premature
               decrement allows the slot to be reclaimed while the copy is running.

            4. Pending output overflow: MAX_PENDING_OUTPUT=16. If add_pending_output
               fails (table full), verify av1_sync returns AV1_ERROR_QUEUE_FULL and
               does NOT leave a dangling DPB reference.
        """),
    },
    {
        "name": "ivf_parser",
        "files": ["ivf_parser.h", "ivf_parser.c"],
        "context_files": [],
        "aom_context": [],
        "focus": textwrap.dedent("""\
            Pay special attention to:
            - Endianness: IVF frame size and timestamp are little-endian 32/64-bit.
              Verify the parser reads them correctly on big-endian systems too
              (use explicit LE reads, not raw casts).
            - Buffer bounds: verify that reading frame_size bytes doesn't read past
              the file if the file is truncated.
            - The fourcc field must be checked for 'AV01' to validate it's AV1.
            - Frame iteration: verify the parser correctly advances the file position
              by exactly (4 + 8 + frame_size) bytes per frame.
        """),
    },
    {
        "name": "y4m_writer",
        "files": ["y4m_writer.h", "y4m_writer.c"],
        "context_files": [],
        "aom_context": [],
        "focus": textwrap.dedent("""\
            Pay special attention to:
            - Y4M header format: 'YUV4MPEG2 W%d H%d F%d:%d Ip C%s\\n' — verify
              the colorspace string matches the bit depth and chroma subsampling.
              10-bit 4:2:0 is 'C420p10', not just 'C420'.
            - Frame header: must be exactly 'FRAME\\n' (6 bytes) before each frame.
            - Plane write order: Y, then Cb (U), then Cr (V). Verify stride handling
              for cases where stride != width_in_bytes (padding rows).
            - 16-bit samples: for 10/12-bit, each sample is 2 bytes little-endian.
              Verify the writer handles both 8-bit and 16-bit sample sizes.
        """),
    },
    {
        "name": "test_e2e",
        "files": ["test_e2e.c"],
        "context_files": [
            "av1_decoder_api.h",
            "av1_mem_override.h",
            "ivf_parser.h",
            "y4m_writer.h",
        ],
        "aom_context": [],
        "focus": textwrap.dedent("""\
            Pay special attention to:
            - QUEUE_FULL recovery: verify the loop correctly retries av1_decode()
              after draining via sync → set_output → receive_output.
            - Sequence header extraction: the test must parse the AV1 Sequence Header
              OBU from the first frame to fill Av1StreamInfo before calling
              av1_query_memory. Verify this is done correctly.
            - Memory allocation: verify aligned_alloc() is used with the alignment
              from av1_query_memory, not just malloc.
            - Flush drain loop: verify av1_sync() is called in a loop until
              AV1_END_OF_STREAM, not just once.
            - Resource cleanup: verify all allocated memory, file handles, and the
              decoder are properly freed/destroyed on all exit paths including errors.
        """),
    },
]


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def find_aom_base():
    for c in AOM_BASE_DIR_CANDIDATES:
        if c.is_dir() and (c / "aom" / "aom_decoder.h").exists():
            return c
    return None


def load_file(path: Path, start_line: int = None, end_line: int = None) -> str:
    """Load a file, optionally returning only lines [start_line, end_line] (1-indexed, inclusive)."""
    if not path.exists():
        return f"// FILE NOT FOUND: {path}\n"
    text = path.read_text(encoding="utf-8", errors="replace")
    if start_line is None and end_line is None:
        return text
    lines = text.splitlines(keepends=True)
    s = (start_line - 1) if start_line else 0
    e = end_line if end_line else len(lines)
    header = f"// [lines {start_line}–{end_line} of {len(lines)}]\n"
    return header + "".join(lines[s:e])


def send_request(base_url, model, system, messages, max_tokens, temperature, timeout):
    url = f"{base_url.rstrip('/')}/chat/completions"
    payload = {
        "model": model,
        "messages": [{"role": "system", "content": system}] + messages,
        "max_tokens": max_tokens,
        "temperature": temperature,
        "stream": False,
        "tool_choice": "none",
    }
    r = requests.post(url, json=payload, timeout=timeout)
    r.raise_for_status()
    return r.json()


PROGRESS_FILE = "review_progress.md"
MAX_CONTINUATIONS = 3  # max auto-resume attempts per target


def load_progress(output_dir: Path) -> dict:
    """Load progress tracker. Returns {target_name: {status, bugs, elapsed, ...}}."""
    import re
    path = output_dir / PROGRESS_FILE
    if not path.exists():
        return {}
    progress = {}
    for line in path.read_text().splitlines():
        m = re.match(r'- \[([ xX])\] (\S+) — (.+)', line)
        if m:
            done = m.group(1).lower() == 'x'
            name = m.group(2)
            detail = m.group(3)
            progress[name] = {"done": done, "detail": detail}
    return progress


def save_progress(output_dir: Path, progress: dict, all_targets: list):
    """Write progress tracker as markdown checklist."""
    lines = [
        "# AV1 Decoder Code Review Progress\n",
        f"Updated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n",
        "## Targets\n",
    ]
    for t in all_targets:
        name = t["name"]
        info = progress.get(name)
        if info and info["done"]:
            lines.append(f"- [x] {name} — {info['detail']}")
        elif info:
            lines.append(f"- [ ] {name} — {info['detail']}")
        else:
            lines.append(f"- [ ] {name} — PENDING")
    path = output_dir / PROGRESS_FILE
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_progress_header(progress: dict, all_targets: list) -> str:
    """Build a short markdown summary to prepend to prompts."""
    if not progress:
        return ""
    lines = ["## Overall review progress (for your context)\n"]
    for t in all_targets:
        name = t["name"]
        info = progress.get(name)
        if info and info["done"]:
            lines.append(f"- [x] {name} — {info['detail']}")
        elif info:
            lines.append(f"- [ ] {name} — {info['detail']}")
        else:
            lines.append(f"- [ ] {name} — not yet started")
    lines.append("")
    return "\n".join(lines)


def parse_response_checklist(text: str) -> tuple:
    """Parse the model's checklist to find completed/pending focus areas.
    Returns (completed: list[str], pending: list[str])."""
    import re
    completed, pending = [], []
    in_progress = False
    for line in text.splitlines():
        if line.strip().startswith("## Progress"):
            in_progress = True
            continue
        if in_progress:
            m = re.match(r'- \[([ xX])\] (.+)', line)
            if m:
                if m.group(1).lower() == 'x':
                    completed.append(m.group(2))
                else:
                    pending.append(m.group(2))
            elif line.strip() and not line.startswith('-'):
                in_progress = False  # end of checklist
    return completed, pending


def extract_files(text: str, out_dir: Path):
    import re
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "raw_response.md").write_text(text, encoding="utf-8")

    # Match ### filename.ext followed by ```c ... ```
    pattern = re.compile(
        r'###\s+([\w./]+\.[ch])\s*\n+```[ch]?\n(.*?)```',
        re.DOTALL
    )
    saved = []
    for filename, code in pattern.findall(text):
        filename = Path(filename).name
        (out_dir / filename).write_text(code, encoding="utf-8")
        saved.append(filename)

    # Fallback: unnamed fenced blocks
    if not saved:
        unnamed = re.findall(r'```[ch]\n(.*?)```', text, re.DOTALL)
        for i, code in enumerate(unnamed):
            name = f"unnamed_{i+1}.c"
            (out_dir / name).write_text(code, encoding="utf-8")
            saved.append(name)

    return saved


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="http://192.168.1.120:8000/v1")
    parser.add_argument("--model", default=MODEL_NAME)
    parser.add_argument("--max-tokens", type=int, default=MAX_TOKENS)
    parser.add_argument("--temperature", type=float, default=TEMPERATURE)
    parser.add_argument("--timeout", type=int, default=REQUEST_TIMEOUT)
    parser.add_argument("--only", default=None, help="Review only this target name")
    parser.add_argument("--output-dir", default=str(OUTPUT_DIR))
    parser.add_argument("--aom-dir", default=None)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    aom_base = Path(args.aom_dir) if args.aom_dir else find_aom_base()

    targets = REVIEW_TARGETS
    if args.only:
        targets = [t for t in REVIEW_TARGETS if t["name"] == args.only]
        if not targets:
            print(f"ERROR: no target named '{args.only}'")
            sys.exit(1)

    log_path = output_dir / "review_log.txt"
    def log(msg):
        ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        line = f"[{ts}] {msg}"
        print(line)
        with open(log_path, "a") as f:
            f.write(line + "\n")

    # Load or initialise overall progress
    progress = load_progress(output_dir)
    all_targets = REVIEW_TARGETS  # full list for progress display

    log("=== AV1 Decoder Code Review Runner ===")
    log(f"Base URL: {args.base_url}")
    log(f"Model: {args.model}")
    log(f"Merge dir: {MERGE_DIR}")
    log(f"Output dir: {output_dir}")
    log(f"AOM base: {aom_base or 'NOT FOUND'}")
    log(f"Targets: {[t['name'] for t in targets]}")
    done_count = sum(1 for v in progress.values() if v.get("done"))
    log(f"Progress: {done_count}/{len(all_targets)} targets completed previously")
    log("")

    for target in targets:
        name = target["name"]
        out_dir = output_dir / name

        # Skip already-completed targets (unless --only forces a re-run)
        if not args.only and progress.get(name, {}).get("done"):
            log(f"SKIPPING: {name} — already completed ({progress[name]['detail']})")
            log("")
            continue

        log(f"{'='*60}")
        log(f"REVIEWING: {name}")
        log(f"{'='*60}")

        # Build context message
        context_parts = []

        # Load AOM source context
        if aom_base:
            for rel in target.get("aom_context", []):
                path = aom_base / rel
                content = load_file(path)
                context_parts.append(f"### {rel} (AOM reference)\n```c\n{content}\n```")

        # Load sibling files for context
        for dep_file in target.get("context_files", []):
            path = MERGE_DIR / dep_file
            content = load_file(path)
            context_parts.append(f"### {dep_file} (dependency)\n```c\n{content}\n```")

        # Load the files to review (optionally sliced by line range)
        line_ranges = target.get("line_ranges", {})  # {"filename": (start, end)}
        review_parts = []
        for fname in target["files"]:
            path = MERGE_DIR / fname
            lr = line_ranges.get(fname)
            if lr:
                content = load_file(path, start_line=lr[0], end_line=lr[1])
                label = f"{fname} (lines {lr[0]}–{lr[1]})"
            else:
                content = load_file(path)
                label = fname
            review_parts.append(f"### {label}\n```c\n{content}\n```")

        # Build progress header for the model
        progress_header = build_progress_header(progress, all_targets)

        messages = []

        if context_parts:
            messages.append({
                "role": "user",
                "content": "Here is context you may need for the review:\n\n" + "\n\n".join(context_parts)
            })
            messages.append({
                "role": "assistant",
                "content": "Understood. I have reviewed the context files."
            })

        review_body = (
            progress_header
            + f"Please review the following generated C code for the AV1 decoder API.\n\n"
            f"## Files to review\n\n"
            + "\n\n".join(review_parts)
            + "\n\n## Focus areas\n\n"
            + target["focus"]
            + "\n\nFor each bug you find: state the file, approximate line number, "
            "the problem, and the fix. Then output corrected files (only files that "
            "have bugs) using ### filename.ext fenced code blocks. "
            "If a file has no bugs, say so explicitly. "
            "Do NOT output tool calls or XML."
        )
        messages.append({"role": "user", "content": review_body})

        total_chars = sum(len(m["content"]) for m in messages)
        est_tokens = total_chars // 3
        log(f"  Input: {len(messages)} messages, ~{total_chars:,} chars (~{est_tokens:,} tokens)")

        if args.dry_run:
            log(f"  [DRY RUN] skipping send")
            log("")
            continue

        effective_timeout = target.get("timeout_override", args.timeout)

        # --- Send with auto-continuation on truncation ---
        accumulated_text = ""
        attempt = 0
        final_finish = None
        final_usage = {}
        total_elapsed = 0.0

        while attempt <= MAX_CONTINUATIONS:
            current_messages = list(messages)  # copy

            # If continuing a truncated response, append prior output + continue prompt
            if attempt > 0:
                log(f"  Continuation attempt {attempt}/{MAX_CONTINUATIONS}...")
                completed, pending = parse_response_checklist(accumulated_text)
                log(f"    Completed focus areas: {len(completed)}, Pending: {len(pending)}")

                if not pending:
                    log(f"    All focus areas done — just needs corrected files.")
                    continue_prompt = (
                        "Your previous response was truncated. The checklist shows all "
                        "focus areas are reviewed. Please output ONLY the corrected "
                        "file(s) using ### filename.ext fenced code blocks."
                    )
                else:
                    pending_list = "\n".join(f"- {p}" for p in pending)
                    continue_prompt = (
                        f"Your previous response was truncated. You completed "
                        f"{len(completed)} of {len(completed)+len(pending)} focus areas.\n\n"
                        f"**Remaining focus areas:**\n{pending_list}\n\n"
                        f"Please continue reviewing the remaining areas, then output "
                        f"corrected file(s) using ### filename.ext fenced code blocks."
                    )

                current_messages.append({
                    "role": "assistant",
                    "content": accumulated_text
                })
                current_messages.append({
                    "role": "user",
                    "content": continue_prompt
                })

            start = time.time()
            try:
                resp = send_request(
                    args.base_url, args.model, SYSTEM_PROMPT, current_messages,
                    args.max_tokens, args.temperature, effective_timeout
                )
                elapsed = time.time() - start
                total_elapsed += elapsed
                choice = resp["choices"][0]
                text = choice["message"]["content"]
                finish = choice.get("finish_reason", "?")
                usage = resp.get("usage", {})
                final_finish = finish
                final_usage = usage

                log(f"  Response in {elapsed:.1f}s — {finish} — "
                    f"tokens: {usage.get('total_tokens', '?')}")

                accumulated_text += ("\n" if accumulated_text else "") + text

                if finish == "length" and attempt < MAX_CONTINUATIONS:
                    log(f"  ⚠ TRUNCATED — will auto-continue")
                    attempt += 1
                    continue
                elif finish == "length":
                    log(f"  ⚠ TRUNCATED — max continuations reached, saving partial")
                break

            except requests.exceptions.Timeout:
                total_elapsed += time.time() - start
                log(f"  ✗ TIMEOUT after {time.time()-start:.0f}s")
                progress[name] = {"done": False, "detail": f"TIMEOUT ({total_elapsed:.0f}s)"}
                save_progress(output_dir, progress, all_targets)
                break
            except requests.exceptions.ConnectionError as e:
                log(f"  ✗ CONNECTION ERROR: {e}")
                sys.exit(1)
            except requests.exceptions.HTTPError as e:
                log(f"  ✗ HTTP ERROR: {e}")
                progress[name] = {"done": False, "detail": f"HTTP ERROR"}
                save_progress(output_dir, progress, all_targets)
                break
            except Exception as e:
                log(f"  ✗ ERROR: {type(e).__name__}: {e}")
                progress[name] = {"done": False, "detail": f"ERROR: {e}"}
                save_progress(output_dir, progress, all_targets)
                break
        else:
            # while-else: only runs if loop completed without break
            pass

        # Save accumulated output
        if accumulated_text:
            saved = extract_files(accumulated_text, out_dir)
            log(f"  Saved: {saved if saved else ['raw_response.md only']}")

            # Count bugs from checklist
            completed, pending = parse_response_checklist(accumulated_text)
            bug_summary = f"{len(completed)} areas reviewed"
            if pending:
                bug_summary += f", {len(pending)} incomplete"

            meta = {
                "target": name,
                "timestamp": datetime.now().isoformat(),
                "elapsed_seconds": total_elapsed,
                "finish_reason": final_finish,
                "continuations": attempt,
                "usage": final_usage,
                "corrected_files": saved,
                "completed_areas": completed,
                "pending_areas": pending,
                "response_hash": hashlib.sha256(accumulated_text.encode()).hexdigest()[:16],
            }
            (out_dir / "metadata.json").write_text(json.dumps(meta, indent=2))

            is_done = final_finish in ("stop", "end_turn") or (not pending and completed)
            progress[name] = {
                "done": is_done,
                "detail": f"DONE ({bug_summary}, {total_elapsed:.0f}s)" if is_done
                         else f"PARTIAL ({bug_summary}, {total_elapsed:.0f}s)",
            }
        else:
            progress[name] = {"done": False, "detail": "NO RESPONSE"}

        save_progress(output_dir, progress, all_targets)
        log("")

    log("=== Review complete ===")
    done_count = sum(1 for v in progress.values() if v.get("done"))
    log(f"Progress: {done_count}/{len(all_targets)} targets completed")
    log(f"Outputs: {output_dir}")
    log(f"Progress file: {output_dir / PROGRESS_FILE}")


if __name__ == "__main__":
    main()
