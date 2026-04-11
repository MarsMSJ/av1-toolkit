#!/usr/bin/env python3
"""
Follow-up verification script for AV1 decoder prompt outputs.

Phase 1: Per-prompt code review against checklists (standalone, no AOM dep)
Phase 2: Cross-prompt API consistency check (headers must agree)
Phase 3: Merge into unified source tree + generate Makefile

Reuses the streaming infrastructure from run_prompts.py.

Usage:
    python run_verification.py --base-url http://sparks:8000/v1
    python run_verification.py --phase 1          # run only phase 1
    python run_verification.py --only 3           # review only prompt 3
    python run_verification.py --dry-run
"""

import argparse
import json
import os
import sys
import time
import hashlib
from datetime import datetime
from pathlib import Path

try:
    import requests
except ImportError:
    print("ERROR: 'requests' not installed. Run: pip install requests")
    sys.exit(1)

# Import streaming helper from the prompt runner
SCRIPT_DIR = Path(__file__).parent.resolve()
sys.path.insert(0, str(SCRIPT_DIR))
from run_prompts import (
    send_prompt_streaming, extract_and_save_files,
    PROMPTS, MODEL_NAME, MAX_TOKENS, TEMPERATURE, REQUEST_TIMEOUT,
)

OUTPUT_DIR = SCRIPT_DIR / "prompt_outputs"
VERIFY_DIR = SCRIPT_DIR / "verification_outputs"

# ---------------------------------------------------------------------------
# System prompt for verification
# ---------------------------------------------------------------------------

VERIFY_SYSTEM_PROMPT = """\
You are a senior C systems programmer performing a code review.

You will be given C source files that were generated for an AV1 video decoder
project. Your job is to review them for correctness, completeness, and
adherence to the requirements.

For each issue found, provide:
1. Severity: CRITICAL / WARNING / STYLE
2. File and approximate location
3. What's wrong
4. How to fix it

At the end, provide a CORRECTED version of any files that have CRITICAL or
WARNING issues. Output the corrected files using:

### filename.h
```c
// corrected file contents
```

If no corrections are needed, say "NO CORRECTIONS NEEDED" and explain why
the code is acceptable.

Be thorough but practical — this is reference/prototype code, not production.
Focus on: correctness, thread safety, memory safety, API contract violations.
"""

MERGE_SYSTEM_PROMPT = """\
You are a senior C systems programmer. You are NOT an agent — you have NO \
tools, NO file access. You can ONLY output text.

You will be given C source files from multiple implementation prompts for an \
AV1 video decoder. Some files (especially av1_decoder_api.h and \
av1_decoder_api.c) were produced by different prompts and may have \
conflicting or duplicated definitions.

Your job: produce a SINGLE unified set of source files that:
1. Merges all APIs into one consistent av1_decoder_api.h / av1_decoder_api.c
2. Resolves any conflicts between prompt outputs (later prompts take priority)
3. Ensures all #includes are correct
4. Produces a Makefile that compiles everything

Output the COMPLETE merged files using:
### filename.h
```c
// complete file contents
```

Also output a Makefile that compiles:
- Each test individually (test_mem_override, test_job_queue, etc.)
- The end-to-end test linking everything together
- With flags: -std=c11 -Wall -Wextra -pthread -I. -laom
"""


# ---------------------------------------------------------------------------
# Load generated code files for a prompt
# ---------------------------------------------------------------------------

def load_prompt_code(prompt_id: int, output_dir: Path) -> dict:
    """Load the named .h and .c files (not unnamed_*) for a prompt."""
    prompt_info = next((p for p in PROMPTS if p["id"] == prompt_id), None)
    if not prompt_info:
        return {}

    prompt_dir = output_dir / f"prompt_{prompt_id:02d}_{prompt_info['name']}"
    if not prompt_dir.exists():
        return {}

    files = {}
    for f in sorted(prompt_dir.glob("*.[ch]")):
        if f.name.startswith("unnamed_"):
            continue
        files[f.name] = f.read_text(encoding="utf-8", errors="replace")

    return files


def format_code_for_review(files: dict) -> str:
    """Format code files into a markdown block for the model."""
    sections = []
    for name, content in sorted(files.items()):
        sections.append(f"### {name}\n```c\n{content}\n```")
    return "\n\n".join(sections)


# ---------------------------------------------------------------------------
# Phase 1: Per-prompt review
# ---------------------------------------------------------------------------

def build_phase1_prompts(output_dir: Path, only_id: int = None):
    """Build review prompts for each prompt's output."""
    reviews = []

    for p in PROMPTS:
        if only_id and p["id"] != only_id:
            continue

        files = load_prompt_code(p["id"], output_dir)
        if not files:
            continue

        # Load checklist from metadata
        meta_path = output_dir / f"prompt_{p['id']:02d}_{p['name']}" / "metadata.json"
        checklist = p.get("checklist", [])
        if meta_path.exists():
            try:
                meta = json.loads(meta_path.read_text(encoding="utf-8"))
                checklist = meta.get("checklist", checklist)
            except Exception:
                pass

        code_block = format_code_for_review(files)
        checklist_block = "\n".join(f"- [ ] {item}" for item in checklist)

        prompt = (
            f"## Review: Prompt {p['id']} — {p['title']}\n\n"
            f"### Requirements Checklist\n{checklist_block}\n\n"
            f"### Generated Code ({len(files)} files)\n\n{code_block}\n\n"
            f"Review this code against the checklist above. For each checklist item, "
            f"mark it PASS or FAIL with a brief explanation. Then list any additional "
            f"issues found.\n\n"
            f"If any files need corrections, output the CORRECTED files in full."
        )

        reviews.append({
            "id": p["id"],
            "name": p["name"],
            "title": p["title"],
            "prompt": prompt,
            "files": files,
        })

    return reviews


# ---------------------------------------------------------------------------
# Phase 2: Cross-prompt consistency
# ---------------------------------------------------------------------------

def build_phase2_prompt(output_dir: Path):
    """Build a prompt that checks API consistency across all prompts."""
    # Collect all av1_decoder_api.h versions
    api_headers = {}
    api_impls = {}
    all_headers = {}

    for p in PROMPTS:
        files = load_prompt_code(p["id"], output_dir)
        for fname, content in files.items():
            if fname == "av1_decoder_api.h":
                api_headers[f"prompt_{p['id']:02d}"] = content
            elif fname == "av1_decoder_api.c":
                api_impls[f"prompt_{p['id']:02d}"] = content
            elif fname.endswith(".h"):
                all_headers[fname] = content

    if len(api_headers) < 2:
        return None

    sections = []
    sections.append("## av1_decoder_api.h versions across prompts\n")
    for key, content in sorted(api_headers.items()):
        sections.append(f"### {key}/av1_decoder_api.h\n```c\n{content}\n```")

    sections.append("\n## Other headers for reference\n")
    for fname, content in sorted(all_headers.items()):
        sections.append(f"### {fname}\n```c\n{content}\n```")

    code_block = "\n\n".join(sections)

    prompt = (
        f"{code_block}\n\n"
        f"The av1_decoder_api.h header was produced by multiple prompts. Each prompt "
        f"added new functions/types. Check for:\n"
        f"1. Conflicting struct definitions (same name, different fields)\n"
        f"2. Missing forward declarations\n"
        f"3. Inconsistent enum values\n"
        f"4. Functions declared in headers but never implemented\n"
        f"5. #include dependency issues\n\n"
        f"Produce a UNIFIED av1_decoder_api.h that merges all versions correctly, "
        f"with the most complete definition of each struct/enum/function."
    )

    return {
        "id": "consistency",
        "name": "api_consistency",
        "title": "Cross-Prompt API Consistency Check",
        "prompt": prompt,
    }


# ---------------------------------------------------------------------------
# Phase 3: Merge into unified source tree
# ---------------------------------------------------------------------------

def build_phase3_prompt(output_dir: Path):
    """Build a prompt to merge all code into a unified compilable tree."""
    all_files = {}

    # Collect the "best" version of each file (later prompts override earlier)
    for p in PROMPTS:
        files = load_prompt_code(p["id"], output_dir)
        for fname, content in files.items():
            # Skip test files — we'll include them separately
            if fname.startswith("test_"):
                all_files[f"tests/{fname}"] = content
            else:
                all_files[fname] = content

    sections = []
    for path, content in sorted(all_files.items()):
        sections.append(f"### {path}\n```c\n{content}\n```")

    code_block = "\n\n".join(sections)

    prompt = (
        f"## All generated source files\n\n{code_block}\n\n"
        f"Merge these into a unified, compilable source tree. Specifically:\n\n"
        f"1. **av1_decoder_api.h** — merge all versions into one definitive header\n"
        f"2. **av1_decoder_api.c** — merge all function implementations (query, create, "
        f"decode, sync, set_output, receive_output, flush, destroy)\n"
        f"3. Fix any #include issues so all files find their dependencies\n"
        f"4. Produce a **Makefile** with targets:\n"
        f"   - `test_mem`: standalone memory override test\n"
        f"   - `test_queue`: standalone job queue test\n"
        f"   - `test_copy`: standalone copy thread test\n"
        f"   - `test_gpu`: standalone GPU thread stub test\n"
        f"   - `test_e2e`: full end-to-end test linking everything + libaom\n"
        f"   - `all`: build everything\n"
        f"   - `clean`: remove build artifacts\n"
        f"   Compiler flags: -std=c11 -Wall -Wextra -Wpedantic -pthread\n"
        f"   Link flags for e2e: -laom -lm\n\n"
        f"Output ALL files in full. Do not abbreviate or skip any file."
    )

    return {
        "id": "merge",
        "name": "merged_source",
        "title": "Unified Source Tree + Makefile",
        "prompt": prompt,
    }


# ---------------------------------------------------------------------------
# Main runner
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Verify and merge AV1 decoder prompt outputs")
    parser.add_argument("--base-url", default="http://192.168.1.120:8000/v1",
                        help="OpenAI-compatible API base URL (default: http://192.168.1.120:8000/v1)")
    parser.add_argument("--model", default=MODEL_NAME,
                        help=f"Model name (default: {MODEL_NAME})")
    parser.add_argument("--max-tokens", type=int, default=MAX_TOKENS,
                        help=f"Max output tokens (default: {MAX_TOKENS})")
    parser.add_argument("--temperature", type=float, default=TEMPERATURE,
                        help=f"Sampling temperature (default: {TEMPERATURE})")
    parser.add_argument("--timeout", type=int, default=REQUEST_TIMEOUT,
                        help=f"Request timeout in seconds (default: {REQUEST_TIMEOUT})")
    parser.add_argument("--phase", type=int, default=None, choices=[1, 2, 3],
                        help="Run only this phase (1=review, 2=consistency, 3=merge)")
    parser.add_argument("--only", type=int, default=None,
                        help="Phase 1 only: review only prompt N")
    parser.add_argument("--dry-run", action="store_true",
                        help="Print prompts without sending")
    parser.add_argument("--output-dir", type=str, default=str(OUTPUT_DIR),
                        help=f"Prompt outputs directory (default: {OUTPUT_DIR})")
    parser.add_argument("--verify-dir", type=str, default=str(VERIFY_DIR),
                        help=f"Verification outputs directory (default: {VERIFY_DIR})")
    parser.add_argument("--max-continuations", type=int, default=3,
                        help="Max continuation requests per prompt (default: 3)")
    args = parser.parse_args()

    output_dir = Path(args.output_dir)
    verify_dir = Path(args.verify_dir)
    verify_dir.mkdir(parents=True, exist_ok=True)

    log_path = verify_dir / "verify_log.txt"
    def log(msg):
        ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        line = f"[{ts}] {msg}"
        print(line)
        with open(log_path, "a", encoding="utf-8") as f:
            f.write(line + "\n")

    phases_to_run = [args.phase] if args.phase else [1, 2, 3]

    log("=== AV1 Decoder Verification Runner ===")
    log(f"Base URL: {args.base_url}")
    log(f"Model: {args.model}")
    log(f"Phases: {phases_to_run}")
    log(f"Prompt outputs: {output_dir}")
    log(f"Verification outputs: {verify_dir}")
    log("")

    # Check connectivity
    if not args.dry_run:
        try:
            r = requests.get(f"{args.base_url.rstrip('/')}/models", timeout=10)
            r.raise_for_status()
            log(f"Connected to vLLM.")
        except Exception as e:
            log(f"WARNING: Could not connect: {e}")
        log("")

    # Collect all tasks across phases
    tasks = []

    if 1 in phases_to_run:
        reviews = build_phase1_prompts(output_dir, only_id=args.only)
        for r in reviews:
            tasks.append({
                "phase": 1,
                "id": f"review_{r['id']:02d}_{r['name']}" if isinstance(r['id'], int) else r['id'],
                "title": f"Phase 1 Review: {r['title']}",
                "system": VERIFY_SYSTEM_PROMPT,
                "prompt": r["prompt"],
            })

    if 2 in phases_to_run:
        consistency = build_phase2_prompt(output_dir)
        if consistency:
            tasks.append({
                "phase": 2,
                "id": consistency["id"],
                "title": f"Phase 2: {consistency['title']}",
                "system": VERIFY_SYSTEM_PROMPT,
                "prompt": consistency["prompt"],
            })

    if 3 in phases_to_run:
        merge = build_phase3_prompt(output_dir)
        tasks.append({
            "phase": 3,
            "id": merge["id"],
            "title": f"Phase 3: {merge['title']}",
            "system": MERGE_SYSTEM_PROMPT,
            "prompt": merge["prompt"],
        })

    log(f"Total tasks: {len(tasks)}")
    log("")

    # Run tasks
    for task in tasks:
        task_dir = verify_dir / task["id"]
        task_dir.mkdir(parents=True, exist_ok=True)

        log(f"{'='*60}")
        log(f"{task['title']}")
        log(f"{'='*60}")

        messages = [{"role": "user", "content": task["prompt"]}]

        if args.dry_run:
            total_chars = len(task["prompt"])
            log(f"  [DRY RUN] Would send {total_chars:,} chars (~{total_chars // 3:,} tokens)")
            log("")
            continue

        start_time = time.time()
        full_text = ""
        continuation = 0
        cur_messages = list(messages)
        total_usage = {"prompt_tokens": 0, "completion_tokens": 0, "total_tokens": 0}
        final_finish_reason = None

        def save_task_results(reason):
            task_dir.mkdir(parents=True, exist_ok=True)
            saved_files = []
            if full_text.strip():
                saved_files = extract_and_save_files(full_text, task_dir)
                log(f"  Saved: {saved_files}")

            meta = {
                "task_id": task["id"],
                "phase": task["phase"],
                "title": task["title"],
                "timestamp": datetime.now().isoformat(),
                "elapsed_seconds": time.time() - start_time,
                "finish_reason": reason,
                "usage": total_usage,
                "continuations": continuation,
                "model": args.model,
                "extracted_files": saved_files,
                "response_hash": hashlib.sha256(full_text.encode()).hexdigest()[:16] if full_text else "",
            }
            (task_dir / "metadata.json").write_text(
                json.dumps(meta, indent=2), encoding="utf-8"
            )
            log(f"  Metadata saved: finish_reason={reason}")

        try:
            while continuation <= args.max_continuations:
                label = f"(continuation {continuation})" if continuation > 0 else ""
                log(f"  Sending ({len(cur_messages)} messages) {label}...")

                response = send_prompt_streaming(
                    base_url=args.base_url,
                    model=args.model,
                    system=task["system"],
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

                for k in total_usage:
                    total_usage[k] += usage.get(k, 0)

                log(f"  Response in {elapsed:.1f}s — {finish_reason} — "
                    f"tokens: {usage.get('completion_tokens', '?')}")

                full_text += text

                if finish_reason in ("length", "timeout") and text.strip():
                    continuation += 1
                    if continuation > args.max_continuations:
                        log(f"  Hit max continuations ({args.max_continuations})")
                        final_finish_reason = f"{finish_reason}_max_continuations"
                        break

                    checkpoint_path = task_dir / f"checkpoint_{continuation}.md"
                    checkpoint_path.write_text(full_text, encoding="utf-8")
                    log(f"  Checkpoint saved ({len(full_text)} chars), continuing...")

                    cur_messages = list(messages)
                    cur_messages.append({"role": "assistant", "content": full_text})
                    cur_messages.append({
                        "role": "user",
                        "content": (
                            "Your previous response was cut off. Continue EXACTLY where you "
                            "left off. Do not repeat anything. Pick up from the last line."
                        )
                    })
                elif finish_reason == "timeout" and not text.strip():
                    log(f"  TIMEOUT with no output after {elapsed:.1f}s")
                    final_finish_reason = "timeout_empty"
                    break
                else:
                    final_finish_reason = finish_reason
                    break

            save_task_results(final_finish_reason)

        except requests.exceptions.ConnectionError as e:
            log(f"  CONNECTION ERROR: {e}")
            save_task_results(f"connection_error: {e}")
            sys.exit(1)
        except Exception as e:
            log(f"  ERROR: {type(e).__name__}: {e}")
            save_task_results(f"error: {type(e).__name__}: {e}")
            continue

        log("")

    log("=== Verification complete ===")
    log(f"Outputs: {verify_dir}")


if __name__ == "__main__":
    main()
