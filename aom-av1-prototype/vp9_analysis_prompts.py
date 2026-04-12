#!/usr/bin/env python3
"""
VP9 diff analysis and scrub runner for Qwen 3 Coder Next.

Two-stage pipeline:
  Stage 1 (vp9_diff_analysis): Compare proprietary VP9 port vs libvpx reference.
           Requires --libvpx-dir and --custom-vp9-dir pointing at the codebases.
  Stage 2 (scrub_report): Sanitize the raw report, removing proprietary terms.
           Automatically feeds Stage 1 output as input.

Usage:
    python vp9_analysis_prompts.py                              # run both stages
    python vp9_analysis_prompts.py --only vp9_diff_analysis     # stage 1 only
    python vp9_analysis_prompts.py --only scrub_report          # stage 2 only (needs prior stage 1 output)
    python vp9_analysis_prompts.py --dry-run                    # print without sending
    python vp9_analysis_prompts.py --resume                     # skip completed stages

Designed for Qwen 3 Coder Next running on vLLM.
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
OUTPUT_DIR    = SCRIPT_DIR / "vp9_analysis_outputs"
PROGRESS_FILE = "vp9_progress.md"
MODEL_NAME    = "MiniMax-M2.5"
MAX_TOKENS    = 16384
TEMPERATURE   = 0.15
REQUEST_TIMEOUT = 3600  # 1 hour per stage
MAX_CONTINUATIONS = 3

# ---------------------------------------------------------------------------
# Source file patterns to collect from each codebase
# We don't send the entire repo — just the key decoder files.
# ---------------------------------------------------------------------------

# libvpx key files (relative to libvpx root)
LIBVPX_KEY_FILES = [
    "vpx/vpx_decoder.h",
    "vpx/vpx_codec.h",
    "vpx_mem/vpx_mem.h",
    "vpx_mem/vpx_mem.c",
    "vp9/common/vp9_common.h",
    "vp9/decoder/vp9_decoder.h",
    "vp9/decoder/vp9_decoder.c",
    "vp9/decoder/vp9_decodeframe.h",
    "vp9/decoder/vp9_decodeframe.c",
    "vp9/decoder/vp9_dthread.h",
    "vp9/decoder/vp9_dthread.c",
    "vpx_util/vpx_thread.h",
    "vpx_util/vpx_thread.c",
    "vp9/common/vp9_alloccommon.h",
    "vp9/common/vp9_alloccommon.c",
]

# Custom VP9 port — try to auto-discover, or specify with --custom-vp9-dir
CUSTOM_VP9_GLOBS = [
    "**/*decode*.c",
    "**/*decode*.h",
    "**/*thread*.c",
    "**/*thread*.h",
    "**/*queue*.c",
    "**/*queue*.h",
    "**/*mem*.c",
    "**/*mem*.h",
    "**/*api*.c",
    "**/*api*.h",
    "**/*dpb*.c",
    "**/*dpb*.h",
    "**/*copy*.c",
    "**/*copy*.h",
]

# Maximum chars from a single source file (to avoid blowing context)
MAX_FILE_CHARS = 30000

# ---------------------------------------------------------------------------
# Proprietary prefix blocklist — EDIT BEFORE RUNNING
# ---------------------------------------------------------------------------

PROPRIETARY_PREFIXES = [
    "lux_",
    "sce_",
    # Add more prefixes here
]

PROPRIETARY_TERMS = [
    # Add proprietary project names, console names, SDK names, team names here
    # e.g., "ProjectOrion", "PS5", "GNM", etc.
]

# ---------------------------------------------------------------------------
# System prompts
# ---------------------------------------------------------------------------

DIFF_ANALYSIS_SYSTEM = """\
You are a senior video codec engineer performing a detailed code review. \
You are NOT an agent — you have NO tools, NO file access. Output only text.

You will be given source files from two VP9 decoder codebases and asked to \
produce a detailed diff analysis organized by functional area.

## Output format — use this exact structure

Start your response with a markdown checklist of the 10 analysis areas. \
Mark each as you complete it. This lets the runner track progress if your \
output is interrupted and needs to be resumed.

```
## Progress
- [x] 1. Public API Layer — analyzed
- [x] 2. Memory Management — analyzed
- [ ] 3. Threading Architecture — not yet analyzed
...
```

Then for each area, provide:
1. **Reference (libvpx)**: Brief description of how the reference handles this
2. **Custom**: Detailed description of the custom approach
3. **Key files changed**: List of files and the nature of changes
4. **Diff summary**: Pseudo-diff showing the conceptual before/after
5. **Design rationale**: Why this change was likely made

Do NOT output tool calls or XML. Be exhaustive — do not skip minor changes. \
Flag potential bugs, race conditions, workarounds, and TODOs.
"""

SCRUB_SYSTEM = """\
You are a technical editor specializing in video codec documentation. \
You are NOT an agent — you have NO tools, NO file access. Output only text.

You will be given a raw diff analysis report containing PROPRIETARY terms. \
Your task is to scrub it so it contains ONLY public libvpx terminology.

## Output format — use this exact structure

Start your response with a markdown checklist of the sections being scrubbed. \
Mark each as you complete it.

```
## Progress
- [x] 1. Public API Layer — scrubbed
- [x] 2. Memory Management — scrubbed
- [ ] 3. Threading Architecture — not yet scrubbed
...
```

For each section output:
1. **libvpx reference behavior** — keep as-is (public knowledge)
2. **Architectural pattern used** — describe using ONLY generic/libvpx terms
3. **Design rationale** — keep, scrub proprietary details
4. **Applicability to AV1** — ADD this subsection mapping patterns to AOM equivalents

## Final verification
At the end, output a verification section:
```
## Scrub Verification
- Regex `(?i)(PREFIX)\\w+` matches: 0 (for each proprietary prefix)
- [ ] No proprietary function/struct/type names remain
- [ ] No console/platform SDK names remain
- [ ] No internal project names or ticket numbers remain
- [ ] Report still makes technical sense for AV1 port planning
```

Do NOT output tool calls or XML.
"""

# ---------------------------------------------------------------------------
# Stage definitions
# ---------------------------------------------------------------------------

ANALYSIS_AREAS = [
    "1. Public API Layer",
    "2. Memory Management",
    "3. Threading Architecture",
    "4. Decode Pipeline Split (Non-Blocking DECODE)",
    "5. Queue / Pipeline Management",
    "6. Reference Frame / DPB Changes",
    "7. Bitstream Parsing Changes",
    "8. Post-Processing / Filtering",
    "9. Output / Copy Path",
    "10. Error Handling & Edge Cases",
]


def build_diff_analysis_prompt(libvpx_files: dict, custom_files: dict) -> str:
    """Build the VP9 diff analysis prompt with injected source files."""
    parts = []

    parts.append("# VP9 Diff Analysis\n")
    parts.append("Compare the CUSTOM VP9 decoder against the libvpx REFERENCE.\n")

    parts.append("## Reference Codebase (libvpx)\n")
    for path, content in libvpx_files.items():
        parts.append(f"### {path}\n```c\n{content}\n```\n")

    parts.append("## Custom VP9 Decoder\n")
    for path, content in custom_files.items():
        parts.append(f"### {path}\n```c\n{content}\n```\n")

    parts.append("## Analysis Areas\n")
    parts.append("Analyze each of the following 10 areas. For each, describe "
                 "what changed, why, and how the custom diverges from reference.\n")
    for area in ANALYSIS_AREAS:
        parts.append(f"- {area}")

    parts.append("\n\n## Additional instructions\n")
    parts.append("- Be exhaustive. Do not skip minor changes.\n")
    parts.append("- If a file was added (not in reference), describe it fully.\n")
    parts.append("- If a file was deleted, note what it did and why.\n")
    parts.append("- Pay special attention to workarounds/hacks.\n")
    parts.append("- Note incomplete areas or TODOs.\n")
    parts.append("- Flag potential bugs or race conditions.\n")

    return "\n".join(parts)


def build_scrub_prompt(raw_report: str) -> str:
    """Build the scrub prompt with the raw report injected."""
    prefix_list = ", ".join(f"`{p}`" for p in PROPRIETARY_PREFIXES)
    term_list = ", ".join(f"`{t}`" for t in PROPRIETARY_TERMS) if PROPRIETARY_TERMS else "(none configured)"

    regex_checks = "\n".join(
        f"- `(?i){re.escape(p)}\\w+` → must return ZERO matches"
        for p in PROPRIETARY_PREFIXES
    )

    prompt = textwrap.dedent(f"""\
        # Scrub Report — Remove Proprietary Terms

        ## Proprietary blocklist

        **Prefixes to scrub**: {prefix_list}
        **Terms to scrub**: {term_list}

        ## Scrubbing rules

        ### MUST REMOVE (replace with generic/libvpx equivalents):
        - All identifiers matching proprietary prefixes → describe by purpose
        - All proprietary function/struct/type names → describe using libvpx terms
        - All proprietary file names/paths → "the custom [purpose] module"
        - All proprietary enum values/error codes → describe generically
        - Console/platform-specific names → "the target platform"
        - Team names, project names, codenames → remove entirely
        - Internal ticket/bug references → remove entirely

        ### MUST KEEP (public knowledge):
        - All libvpx function/struct names (vpx_codec_decode, VP9Decoder, etc.)
        - All VP9 spec terms (superblock, transform, loop filter, tile, etc.)
        - Standard CS terms (mutex, condvar, atomic, ring buffer, DPB, etc.)
        - Generic API pattern names (QUERY MEMORY, CREATE DECODER, DECODE, etc.)

        ### Rewrite style:
        - Describe changes as architectural patterns, not specific implementations
        - Use passive voice for proprietary actions
        - Focus on WHAT and WHY, not the exact proprietary HOW

        ### For each section, ADD:
        - **Applicability to AV1**: Map the VP9 pattern to AOM equivalents
          (VP9Decoder → AV1Decoder, VP9_COMMON → AV1_COMMON,
           vp9_decode_frame → aom_decode_frame_from_obus, etc.)

        ## Verification regex checks
        {regex_checks}

        ## Raw report to scrub

        {raw_report}
    """)
    return prompt


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def load_file(path: Path, max_chars: int = MAX_FILE_CHARS) -> str:
    """Load a file, truncating if too large."""
    if not path.exists():
        return f"// FILE NOT FOUND: {path}\n"
    text = path.read_text(encoding="utf-8", errors="replace")
    if len(text) > max_chars:
        text = text[:max_chars] + f"\n// ... TRUNCATED at {max_chars} chars ...\n"
    return text


def collect_files(base_dir: Path, key_files: list) -> dict:
    """Load specific files from a directory."""
    result = {}
    for rel in key_files:
        path = base_dir / rel
        if path.exists():
            result[rel] = load_file(path)
    return result


def discover_custom_files(base_dir: Path) -> dict:
    """Auto-discover relevant files from the custom VP9 port."""
    result = {}
    seen = set()
    for pattern in CUSTOM_VP9_GLOBS:
        for path in sorted(base_dir.glob(pattern)):
            if path.is_file() and path.suffix in ('.c', '.h') and path not in seen:
                seen.add(path)
                rel = str(path.relative_to(base_dir))
                result[rel] = load_file(path)
    return result


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


def parse_response_checklist(text: str) -> tuple:
    """Parse the model's checklist. Returns (completed, pending) lists."""
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


def load_progress(output_dir: Path) -> dict:
    """Load progress tracker."""
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


def save_progress(output_dir: Path, progress: dict, stage_names: list):
    """Write progress tracker as markdown checklist."""
    lines = [
        "# VP9 Analysis Progress\n",
        f"Updated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n",
        "## Stages\n",
    ]
    for name in stage_names:
        info = progress.get(name)
        if info and info["done"]:
            lines.append(f"- [x] {name} — {info['detail']}")
        elif info:
            lines.append(f"- [ ] {name} — {info['detail']}")
        else:
            lines.append(f"- [ ] {name} — PENDING")
    (output_dir / PROGRESS_FILE).write_text("\n".join(lines) + "\n", encoding="utf-8")


def run_stage(name, system_prompt, user_content, output_dir, args, log, progress, stage_names):
    """Run a single analysis stage with auto-continuation."""
    out_dir = output_dir / name
    out_dir.mkdir(parents=True, exist_ok=True)

    messages = [{"role": "user", "content": user_content}]

    total_chars = sum(len(m["content"]) for m in messages)
    est_tokens = total_chars // 3
    log(f"  Input: {len(messages)} messages, ~{total_chars:,} chars (~{est_tokens:,} tokens)")

    if args.dry_run:
        log(f"  [DRY RUN] skipping send")
        # Save the prompt for inspection
        (out_dir / "prompt_preview.md").write_text(user_content[:5000] + "\n...", encoding="utf-8")
        return None

    accumulated_text = ""
    attempt = 0
    final_finish = None
    final_usage = {}
    total_elapsed = 0.0

    while attempt <= MAX_CONTINUATIONS:
        current_messages = list(messages)

        if attempt > 0:
            log(f"  Continuation attempt {attempt}/{MAX_CONTINUATIONS}...")
            completed, pending = parse_response_checklist(accumulated_text)
            log(f"    Completed: {len(completed)}, Pending: {len(pending)}")

            if not pending:
                continue_prompt = (
                    "Your previous response was truncated. The checklist shows all "
                    "areas are analyzed. Please output any remaining content."
                )
            else:
                pending_list = "\n".join(f"- {p}" for p in pending)
                continue_prompt = (
                    f"Your previous response was truncated. You completed "
                    f"{len(completed)} of {len(completed)+len(pending)} areas.\n\n"
                    f"**Remaining:**\n{pending_list}\n\n"
                    f"Please continue from where you left off."
                )

            current_messages.append({"role": "assistant", "content": accumulated_text})
            current_messages.append({"role": "user", "content": continue_prompt})

        start = time.time()
        try:
            resp = send_request(
                args.base_url, args.model, system_prompt, current_messages,
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
            save_progress(output_dir, progress, stage_names)
            return None
        except requests.exceptions.ConnectionError as e:
            log(f"  ✗ CONNECTION ERROR: {e}")
            progress[name] = {"done": False, "detail": "CONNECTION ERROR"}
            save_progress(output_dir, progress, stage_names)
            return None
        except Exception as e:
            log(f"  ✗ ERROR: {type(e).__name__}: {e}")
            progress[name] = {"done": False, "detail": f"ERROR: {e}"}
            save_progress(output_dir, progress, stage_names)
            return None

    # Save output
    if accumulated_text:
        (out_dir / "raw_response.md").write_text(accumulated_text, encoding="utf-8")

        completed, pending = parse_response_checklist(accumulated_text)
        summary = f"{len(completed)} areas done"
        if pending:
            summary += f", {len(pending)} incomplete"

        meta = {
            "stage": name,
            "timestamp": datetime.now().isoformat(),
            "elapsed_seconds": total_elapsed,
            "finish_reason": final_finish,
            "continuations": attempt,
            "usage": final_usage,
            "completed_areas": completed,
            "pending_areas": pending,
            "response_hash": hashlib.sha256(accumulated_text.encode()).hexdigest()[:16],
        }
        (out_dir / "metadata.json").write_text(json.dumps(meta, indent=2))

        is_done = final_finish in ("stop", "end_turn") or (not pending and completed)
        progress[name] = {
            "done": is_done,
            "detail": f"DONE ({summary}, {total_elapsed:.0f}s)" if is_done
                     else f"PARTIAL ({summary}, {total_elapsed:.0f}s)",
        }
        log(f"  Saved: {out_dir / 'raw_response.md'}")
    else:
        progress[name] = {"done": False, "detail": "NO RESPONSE"}

    save_progress(output_dir, progress, stage_names)
    return accumulated_text


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="VP9 diff analysis and scrub runner for Qwen 3 Coder Next"
    )
    parser.add_argument("--base-url", default="http://192.168.1.120:8000/v1",
                        help="vLLM API base URL")
    parser.add_argument("--model", default=MODEL_NAME,
                        help="Model name for API requests")
    parser.add_argument("--max-tokens", type=int, default=MAX_TOKENS)
    parser.add_argument("--temperature", type=float, default=TEMPERATURE)
    parser.add_argument("--timeout", type=int, default=REQUEST_TIMEOUT)
    parser.add_argument("--only", default=None,
                        choices=["vp9_diff_analysis", "scrub_report"],
                        help="Run only this stage")
    parser.add_argument("--libvpx-dir", default=None,
                        help="Path to libvpx reference source")
    parser.add_argument("--custom-vp9-dir", default=None,
                        help="Path to custom/proprietary VP9 decoder source")
    parser.add_argument("--output-dir", default=str(OUTPUT_DIR))
    parser.add_argument("--resume", action="store_true",
                        help="Skip completed stages")
    parser.add_argument("--dry-run", action="store_true")

    # Proprietary term overrides
    parser.add_argument("--add-prefix", action="append", default=[],
                        help="Add a proprietary prefix to scrub (repeatable)")
    parser.add_argument("--add-term", action="append", default=[],
                        help="Add a proprietary term to scrub (repeatable)")

    args = parser.parse_args()

    # Merge CLI proprietary additions
    for p in args.add_prefix:
        if p not in PROPRIETARY_PREFIXES:
            PROPRIETARY_PREFIXES.append(p)
    for t in args.add_term:
        if t not in PROPRIETARY_TERMS:
            PROPRIETARY_TERMS.append(t)

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    stage_names = ["vp9_diff_analysis", "scrub_report"]
    progress = load_progress(output_dir)

    log_path = output_dir / "vp9_analysis_log.txt"
    def log(msg):
        ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        line = f"[{ts}] {msg}"
        print(line)
        with open(log_path, "a") as f:
            f.write(line + "\n")

    log("=== VP9 Diff Analysis & Scrub Runner ===")
    log(f"Base URL: {args.base_url}")
    log(f"Model: {args.model}")
    log(f"Output dir: {output_dir}")
    log(f"Proprietary prefixes: {PROPRIETARY_PREFIXES}")
    log(f"Proprietary terms: {PROPRIETARY_TERMS}")
    done_count = sum(1 for v in progress.values() if v.get("done"))
    log(f"Progress: {done_count}/{len(stage_names)} stages completed previously")
    log("")

    # ===== Stage 1: VP9 Diff Analysis =====
    if args.only is None or args.only == "vp9_diff_analysis":

        if args.resume and progress.get("vp9_diff_analysis", {}).get("done"):
            log("SKIPPING: vp9_diff_analysis — already completed")
            log("")
        else:
            log("=" * 60)
            log("STAGE 1: VP9 Diff Analysis")
            log("=" * 60)

            # Validate paths
            libvpx_dir = Path(args.libvpx_dir) if args.libvpx_dir else None
            custom_dir = Path(args.custom_vp9_dir) if args.custom_vp9_dir else None

            if not libvpx_dir or not libvpx_dir.is_dir():
                log("  ✗ ERROR: --libvpx-dir is required and must be a valid directory")
                log(f"    Got: {libvpx_dir}")
                if args.only == "vp9_diff_analysis":
                    sys.exit(1)
            elif not custom_dir or not custom_dir.is_dir():
                log("  ✗ ERROR: --custom-vp9-dir is required and must be a valid directory")
                log(f"    Got: {custom_dir}")
                if args.only == "vp9_diff_analysis":
                    sys.exit(1)
            else:
                # Collect source files
                libvpx_files = collect_files(libvpx_dir, LIBVPX_KEY_FILES)
                log(f"  libvpx files loaded: {len(libvpx_files)}")
                for f in libvpx_files:
                    log(f"    {f} ({len(libvpx_files[f]):,} chars)")

                custom_files = discover_custom_files(custom_dir)
                log(f"  Custom VP9 files discovered: {len(custom_files)}")
                for f in custom_files:
                    log(f"    {f} ({len(custom_files[f]):,} chars)")

                if not libvpx_files:
                    log("  ⚠ WARNING: No libvpx files found — check --libvpx-dir path")
                if not custom_files:
                    log("  ⚠ WARNING: No custom VP9 files found — check --custom-vp9-dir path")

                # Build and send
                prompt = build_diff_analysis_prompt(libvpx_files, custom_files)
                run_stage("vp9_diff_analysis", DIFF_ANALYSIS_SYSTEM, prompt,
                         output_dir, args, log, progress, stage_names)

            log("")

    # ===== Stage 2: Scrub Report =====
    if args.only is None or args.only == "scrub_report":

        if args.resume and progress.get("scrub_report", {}).get("done"):
            log("SKIPPING: scrub_report — already completed")
            log("")
        else:
            log("=" * 60)
            log("STAGE 2: Scrub Report")
            log("=" * 60)

            # Load raw report from stage 1
            raw_report_path = output_dir / "vp9_diff_analysis" / "raw_response.md"
            if not raw_report_path.exists():
                log("  ✗ ERROR: No raw report found from Stage 1")
                log(f"    Expected: {raw_report_path}")
                log("    Run Stage 1 first, or place the raw report at the path above.")
                if args.only == "scrub_report":
                    sys.exit(1)
            else:
                raw_report = raw_report_path.read_text(encoding="utf-8")
                log(f"  Raw report loaded: {len(raw_report):,} chars")

                # Build and send
                prompt = build_scrub_prompt(raw_report)
                result = run_stage("scrub_report", SCRUB_SYSTEM, prompt,
                                  output_dir, args, log, progress, stage_names)

                # Post-scrub verification: check for leaked proprietary terms
                if result:
                    leaked = []
                    for prefix in PROPRIETARY_PREFIXES:
                        matches = re.findall(f"(?i){re.escape(prefix)}\\w+", result)
                        if matches:
                            leaked.extend(matches)
                    for term in PROPRIETARY_TERMS:
                        if term.lower() in result.lower():
                            leaked.append(term)

                    if leaked:
                        log(f"  ⚠ SCRUB INCOMPLETE: {len(leaked)} proprietary terms leaked:")
                        for term in leaked[:20]:
                            log(f"    - {term}")
                        log("  Re-run with --only scrub_report or manually fix the output.")
                    else:
                        log("  ✓ Scrub verification passed — no proprietary terms found")

            log("")

    log("=== VP9 analysis complete ===")
    done_count = sum(1 for v in progress.values() if v.get("done"))
    log(f"Progress: {done_count}/{len(stage_names)} stages completed")
    log(f"Outputs: {output_dir}")
    log(f"Progress file: {output_dir / PROGRESS_FILE}")


if __name__ == "__main__":
    main()
