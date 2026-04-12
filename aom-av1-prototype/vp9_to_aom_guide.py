#!/usr/bin/env python3
"""
Generate an AOM AV1 reference decoder modification guide based on
VP9 porting analysis reports.

Takes the 3 scrubbed VP9 analysis reports (16_*, 17_*, 18_*) plus
key AOM decoder source files, and produces a concrete guide for
modifying libaom to support the console-style decoder API.

Usage:
    python vp9_to_aom_guide.py --aom-dir ~/dev-tools/aom
    python vp9_to_aom_guide.py --aom-dir ~/dev-tools/aom --dry-run
    python vp9_to_aom_guide.py --aom-dir ~/dev-tools/aom --report-dir ~/dev-tools/av1-toolkit

Designed for MiniMax-M2.5 on local spark cluster.
"""

import argparse
import json
import hashlib
import re
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

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

SCRIPT_DIR    = Path(__file__).parent.resolve()
OUTPUT_DIR    = SCRIPT_DIR / "aom_mod_guide_output"
MODEL_NAME    = "MiniMax-M2.5"
MAX_TOKENS    = 16384
TEMPERATURE   = 0.15
REQUEST_TIMEOUT = 3600
MAX_CONTINUATIONS = 3

# VP9 analysis report files (in the repo root or --report-dir)
REPORT_FILES = [
    "16_plan_internals.md",
    "17_compile_final_plan.md",
    "18_final_review.md",
]

# AOM source files to include as context (~93K chars, ~31K tokens)
# Headers and small implementation files only — large files referenced by name.
AOM_KEY_FILES = [
    "aom/aom_decoder.h",                   # public decoder API
    "aom/aom_codec.h",                     # codec interface
    "aom/internal/aom_codec_internal.h",    # internal codec structs
    "av1/decoder/decoder.h",               # AV1Decoder struct
    "av1/decoder/decoder.c",               # decoder init/destroy/frame
    "av1/decoder/obu.h",                   # OBU parsing header
    "aom_mem/aom_mem.h",                   # memory API
    "aom_mem/aom_mem.c",                   # memory implementation
    "aom_util/aom_thread.h",              # threading primitives
    "av1/common/alloccommon.h",            # frame buffer allocation
]

# Files too large to include but referenced in the guide
AOM_LARGE_FILES_REFERENCE = [
    ("av1/decoder/obu.c",          "OBU parsing + av1_decode_frame_headers_and_setup / av1_decode_tg_tiles_and_wrapup"),
    ("av1/decoder/decodeframe.c",  "Frame decoding: tile workers, inverse transforms, reconstruction"),
    ("av1/common/av1_common_int.h","AV1_COMMON struct: DPB, cur_frame, ref_frame_map, etc."),
    ("av1/common/alloccommon.c",   "Frame buffer allocation: aom_alloc_frame_buffers"),
]

# Maximum chars per file
MAX_FILE_CHARS = 30000

# ---------------------------------------------------------------------------
# System prompt
# ---------------------------------------------------------------------------

SYSTEM_PROMPT = """\
You are a senior video codec engineer writing a modification guide for the \
AOM AV1 reference decoder (libaom). You are NOT an agent — you have NO tools, \
NO file access. Output only text.

You will be given:
1. Three VP9 analysis reports describing how a VP9 reference decoder was \
   modified to support a console-style API (QUERY MEMORY → CREATE DECODER → \
   DECODE → SYNC → SET OUTPUT → RECEIVE OUTPUT → FLUSH).
2. Key AOM AV1 decoder source files for reference.

Your job: produce a CONCRETE modification guide for libaom that applies \
the same patterns. This is not abstract advice — give specific file names, \
function names, struct names, and describe exactly what to change.

## Output format — use this exact structure

Start with a progress checklist:

```
## Progress
- [x] 1. Public API Layer — done
- [ ] 2. Memory Management — not yet
...
```

Then for each area, use this template:

```
## Area N: <title>

### VP9 Pattern (from reports)
Brief summary of what the VP9 port did.

### AOM Equivalent
Specific AOM files, functions, and structs involved.

### Modifications Required
Step-by-step changes with:
- **File**: exact path (e.g., av1/decoder/decoder.h)
- **What**: what to add/change/remove
- **Why**: rationale from the VP9 experience
- **Gotchas**: pitfalls learned from VP9

### Code Sketch
Pseudocode or C snippets showing the modification pattern.
```

Be specific. Reference actual AOM function names, struct fields, and line-level \
descriptions. The reader should be able to open the AOM source and find exactly \
where to make changes.

Do NOT output tool calls or XML.
"""

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def load_file(path: Path, max_chars: int = MAX_FILE_CHARS) -> str:
    if not path.exists():
        return None
    text = path.read_text(encoding="utf-8", errors="replace")
    if len(text) > max_chars:
        text = text[:max_chars] + f"\n// ... TRUNCATED at {max_chars} chars ...\n"
    return text


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


def parse_checklist(text: str) -> tuple:
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
                in_progress = False
    return completed, pending


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Generate AOM AV1 modification guide from VP9 analysis reports"
    )
    parser.add_argument("--base-url", default="http://192.168.1.120:8000/v1")
    parser.add_argument("--model", default=MODEL_NAME)
    parser.add_argument("--max-tokens", type=int, default=MAX_TOKENS)
    parser.add_argument("--temperature", type=float, default=TEMPERATURE)
    parser.add_argument("--timeout", type=int, default=REQUEST_TIMEOUT)
    parser.add_argument("--aom-dir", required=True,
                        help="Path to AOM source (e.g., ~/dev-tools/aom)")
    parser.add_argument("--report-dir", default=None,
                        help="Directory containing 16_*, 17_*, 18_* reports "
                             "(default: parent of this script's directory)")
    parser.add_argument("--output-dir", default=str(OUTPUT_DIR))
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    aom_dir = Path(args.aom_dir).expanduser()
    report_dir = Path(args.report_dir).expanduser() if args.report_dir else SCRIPT_DIR.parent
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    log_path = output_dir / "guide_log.txt"
    def log(msg):
        ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        line = f"[{ts}] {msg}"
        print(line)
        with open(log_path, "a") as f:
            f.write(line + "\n")

    log("=== AOM AV1 Modification Guide Generator ===")
    log(f"Base URL: {args.base_url}")
    log(f"Model: {args.model}")
    log(f"AOM dir: {aom_dir}")
    log(f"Report dir: {report_dir}")
    log(f"Output dir: {output_dir}")
    log("")

    # --- Load VP9 reports ---
    log("Loading VP9 analysis reports...")
    reports = {}
    for fname in REPORT_FILES:
        content = load_file(report_dir / fname)
        if content:
            reports[fname] = content
            log(f"  {fname}: {len(content):,} chars")
        else:
            # Try spark path: files might be in the repo root
            for alt in [report_dir / fname, SCRIPT_DIR.parent / fname]:
                content = load_file(alt)
                if content:
                    reports[fname] = content
                    log(f"  {fname}: {len(content):,} chars (from {alt})")
                    break
            if fname not in reports:
                log(f"  ✗ {fname}: NOT FOUND")

    if not reports:
        log("ERROR: No VP9 reports found. Check --report-dir path.")
        sys.exit(1)

    # --- Load AOM source ---
    log("\nLoading AOM source files...")
    aom_files = {}
    for rel in AOM_KEY_FILES:
        content = load_file(aom_dir / rel)
        if content:
            aom_files[rel] = content
            log(f"  {rel}: {len(content):,} chars")
        else:
            log(f"  ✗ {rel}: NOT FOUND")

    if not aom_files:
        log("ERROR: No AOM files found. Check --aom-dir path.")
        sys.exit(1)

    # --- Build prompt ---
    log("\nBuilding prompt...")

    # Context message: AOM source
    aom_context = "# AOM AV1 Reference Decoder Source\n\n"
    aom_context += "The following are key files from the AOM reference decoder (libaom).\n\n"
    for rel, content in aom_files.items():
        aom_context += f"### {rel}\n```c\n{content}\n```\n\n"

    aom_context += "## Large files (not included, referenced by name)\n\n"
    for rel, desc in AOM_LARGE_FILES_REFERENCE:
        aom_context += f"- **{rel}**: {desc}\n"

    # User message: reports + task
    user_msg = "# VP9 Console Decoder Porting Analysis\n\n"
    user_msg += ("The following reports describe how a VP9 reference decoder (libvpx) "
                 "was modified to support a console-style API. Use these patterns "
                 "as the basis for your AOM modification guide.\n\n")

    for fname, content in reports.items():
        user_msg += f"## {fname}\n\n{content}\n\n---\n\n"

    user_msg += textwrap.dedent("""\
        # Your Task

        Using the VP9 porting patterns above and the AOM source provided as context,
        produce a CONCRETE modification guide for the AOM AV1 reference decoder.

        Cover these 10 areas (same structure as the VP9 analysis):

        1. **Public API Layer** — Map the console API to aom_codec_decode/aom_codec_get_frame.
           Where do av1_decode, av1_sync, av1_set_output, etc. hook in?

        2. **Memory Management** — Replace aom_malloc/aom_memalign/aom_free with pool allocator.
           How to override via aom_mem.h function pointers. Frame buffer allocation strategy.

        3. **Threading Architecture** — Current AOM thread pool vs console thread topology
           (N workers + copy thread + GPU thread). What to keep, what to replace.

        4. **Decode Pipeline Split** — The critical split in obu.c:
           av1_decode_frame_headers_and_setup() stays on caller thread,
           av1_decode_tg_tiles_and_wrapup() moves to workers. Exact functions and call sites.

        5. **Queue / Pipeline Management** — Job queue between caller and workers.
           Ring buffer design. Backpressure (QUEUE_FULL). In-flight frame tracking.

        6. **Reference Frame / DPB Changes** — AOM's RefCntBuffer and ref_frame_map[8].
           When to hold/release refs with async pipeline. DPB slot allocation.

        7. **Bitstream Parsing Changes** — OBU parsing (Temporal Delimiter, Sequence Header,
           Frame Header, Tile Group). What stays on caller thread for the non-blocking constraint.

        8. **Post-Processing / Filtering** — Loop filter, CDEF, loop restoration, film grain.
           Which are on worker threads? Which move to GPU? Film grain on output buffer (not DPB).

        9. **Output / Copy Path** — SET OUTPUT / RECEIVE OUTPUT implementation.
           Copy thread design. Plane-by-plane copy. Format conversion. Film grain as GPU shader.

        10. **Error Handling & Edge Cases** — Resolution change (sequence header change mid-stream).
            Flush drain. Destroy cleanup. Error propagation from worker threads.

        For each area, give:
        - The specific AOM files and functions involved
        - Step-by-step modifications with code sketches
        - Gotchas from the VP9 experience that apply to AV1
    """)

    messages = [
        {"role": "user", "content": aom_context},
        {"role": "assistant", "content": "I've reviewed the AOM source files. Ready for the VP9 reports and task."},
        {"role": "user", "content": user_msg},
    ]

    total_chars = sum(len(m["content"]) for m in messages)
    est_tokens = total_chars // 3
    log(f"Total input: ~{total_chars:,} chars (~{est_tokens:,} tokens)")

    if args.dry_run:
        log("[DRY RUN] skipping send")
        (output_dir / "prompt_preview.md").write_text(
            f"# System Prompt\n\n{SYSTEM_PROMPT}\n\n# AOM Context\n\n{aom_context[:3000]}...\n\n# User Message\n\n{user_msg[:3000]}...",
            encoding="utf-8"
        )
        log(f"Saved prompt preview to {output_dir / 'prompt_preview.md'}")
        return

    # --- Send with auto-continuation ---
    log("\nSending to model...")
    accumulated = ""
    attempt = 0
    total_elapsed = 0.0
    final_finish = None
    final_usage = {}

    while attempt <= MAX_CONTINUATIONS:
        current_messages = list(messages)

        if attempt > 0:
            log(f"Continuation {attempt}/{MAX_CONTINUATIONS}...")
            completed, pending = parse_checklist(accumulated)
            log(f"  Completed: {len(completed)}, Pending: {len(pending)}")

            if pending:
                pending_list = "\n".join(f"- {p}" for p in pending)
                cont = (f"Your response was truncated. You completed {len(completed)} "
                        f"of {len(completed)+len(pending)} areas.\n\n"
                        f"**Remaining:**\n{pending_list}\n\n"
                        f"Continue from where you left off.")
            else:
                cont = ("Your response was truncated but all checklist areas are done. "
                        "Please output any remaining code sketches or summary.")

            current_messages.append({"role": "assistant", "content": accumulated})
            current_messages.append({"role": "user", "content": cont})

        start = time.time()
        try:
            resp = send_request(
                args.base_url, args.model, SYSTEM_PROMPT, current_messages,
                args.max_tokens, args.temperature, args.timeout
            )
            elapsed = time.time() - start
            total_elapsed += elapsed
            choice = resp["choices"][0]
            text = choice["message"]["content"]
            finish = choice.get("finish_reason", "?")
            usage = resp.get("usage", {})
            final_finish = finish
            final_usage = usage

            log(f"Response in {elapsed:.1f}s — {finish} — "
                f"tokens: {usage.get('total_tokens', '?')}")

            accumulated += ("\n" if accumulated else "") + text

            if finish == "length" and attempt < MAX_CONTINUATIONS:
                log("⚠ TRUNCATED — auto-continuing...")
                attempt += 1
                continue
            elif finish == "length":
                log("⚠ TRUNCATED — max continuations reached")
            break

        except requests.exceptions.Timeout:
            log(f"✗ TIMEOUT after {time.time()-start:.0f}s")
            break
        except requests.exceptions.ConnectionError as e:
            log(f"✗ CONNECTION ERROR: {e}")
            sys.exit(1)
        except Exception as e:
            log(f"✗ ERROR: {type(e).__name__}: {e}")
            break

    # --- Save output ---
    if accumulated:
        (output_dir / "aom_modification_guide.md").write_text(accumulated, encoding="utf-8")

        completed, pending = parse_checklist(accumulated)
        meta = {
            "timestamp": datetime.now().isoformat(),
            "elapsed_seconds": total_elapsed,
            "finish_reason": final_finish,
            "continuations": attempt,
            "usage": final_usage,
            "completed_areas": completed,
            "pending_areas": pending,
            "response_hash": hashlib.sha256(accumulated.encode()).hexdigest()[:16],
        }
        (output_dir / "metadata.json").write_text(json.dumps(meta, indent=2))

        log(f"\n✓ Guide saved: {output_dir / 'aom_modification_guide.md'}")
        log(f"  Areas completed: {len(completed)}/{len(completed)+len(pending)}")
    else:
        log("\n✗ No output generated")

    log(f"\nOutputs: {output_dir}")


if __name__ == "__main__":
    main()
