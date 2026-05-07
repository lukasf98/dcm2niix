---
description: Run the in-tree regression suite (dcm_qa, dcm_qa_nih, dcm_qa_uih) against a local dcm2niix build and report pass/fail.
---

Run the three in-tree regression submodules and report a clean pass/fail summary.

## What this command does

1. Confirms a `dcm2niix` binary is available. By default uses `build/bin/dcm2niix` (from the standard CMake build). If `$ARGUMENTS` is non-empty, treat it as the absolute path to the binary to test.
2. Initializes the three submodules if they are empty: `dcm_qa`, `dcm_qa_nih`, `dcm_qa_uih`.
3. Runs `batch.sh` inside each with the chosen binary on `PATH`. Each script uses `set -eu` and internally diffs freshly-converted `Out/` against `Ref/` — a non-zero exit means output drifted.
4. Reports per-submodule pass/fail. On failure, surface the diff and help the user classify it as an **improvement** (update Ref) or an **unintended consequence** (investigate the code change).

## Steps

Use the Bash tool:

```bash
REPO="$(git rev-parse --show-toplevel)"
BIN="${ARGUMENTS:-$REPO/build/bin/dcm2niix}"

if [ ! -x "$BIN" ]; then
    echo "ERROR: $BIN is not executable. Build first (e.g. 'cd build && make') or pass an absolute path: /regressiontest /full/path/to/dcm2niix"
    exit 1
fi

echo "Using dcm2niix: $BIN"
"$BIN" -h 2>&1 | head -1

for d in dcm_qa dcm_qa_nih dcm_qa_uih; do
    if [ -z "$(ls -A "$REPO/$d" 2>/dev/null)" ]; then
        echo "Initializing submodule $d..."
        git -C "$REPO" submodule update --init "$d"
    fi
done

export PATH="$(dirname "$BIN"):$PATH"

pass=0; fail=0; failed_dirs=""
for d in dcm_qa dcm_qa_nih dcm_qa_uih; do
    echo ""
    echo "=== $d ==="
    log="/tmp/regressiontest_$d.log"
    if ( cd "$REPO/$d" && ./batch.sh ) > "$log" 2>&1; then
        echo "PASS: $d"
        pass=$((pass+1))
    else
        echo "FAIL: $d (see $log)"
        fail=$((fail+1))
        failed_dirs="$failed_dirs $d"
    fi
done

echo ""
echo "=== summary ==="
echo "passed: $pass / 3"
echo "failed: $fail / 3"
[ $fail -eq 0 ]
```

## Reporting and failure triage

**On all-pass:** report `All 3 regression suites passed against <BIN>.` and stop.

**On any failure:** for each failed submodule:

1. `tail -60 /tmp/regressiontest_<d>.log` and surface the diff verbatim. Do not paraphrase it — the user knows the data.
2. **Do not assume the change is bad.** A regression-test failure means "the output differs from the saved reference." That can mean either:
   - **Unintended consequence** — the commit broke something. Revert or fix.
   - **Improvement** — the commit legitimately changed output (e.g., corrected a long-standing bug, added a new BIDS field, removed an incorrect one). The `Ref/` files need to be regenerated and re-committed in the submodule.
3. To help the user classify, test the **same binary built from `origin/master`** against the same failing submodule. If master also produces the same diff, the regression **predates this branch** and is not caused by the current work. If master passes and `HEAD` fails, bisect to find the introducing commit:

   ```bash
   # quick master comparison
   git clone --depth 1 --branch master <repo-url> /tmp/dcm2niix_master
   (cd /tmp/dcm2niix_master/console && make) >/dev/null 2>&1
   rm -rf "$REPO/<failed_dir>/Out"
   PATH="/tmp/dcm2niix_master/console:$PATH" bash -c "cd $REPO/<failed_dir> && ./batch.sh" > /tmp/master_<d>.log 2>&1
   echo "master exit: $?"
   ```

4. Present the finding to the user:
   - "HEAD fails but master passes — this branch introduced the regression. Want me to bisect?"
   - "HEAD and master both fail — regression predates this branch; it's not caused by the current work. Flag it for a separate investigation."
   - "The diff looks like an intended improvement (e.g., `+NewField`, `-IncorrectValue`). Want to update `Ref/` in the submodule?"

Let the user decide the classification. Do not revert code or edit `Ref/` files without explicit confirmation.

## Notes

- `Out/` is a local-only artifact. `dcm_qa/.gitignore` already excludes it; `dcm_qa_nih` and `dcm_qa_uih` do not — if those submodules show dirty `Out/` in `git status`, do not commit it; treat it as build output.
- `batch.sh` scripts vary: some decompress `Ref/*.nii.gz` in place, some unzip `In/*.zip`. The first run may leave those side effects in the submodule tree — expected, not a failure.
- The diff ignores `ConversionSoftwareVersion` and `BidsGuess`; drift in any other field is real.
- This is the **minimum** per-commit gate. The full vendor matrix lives in the separate [`dcm_validate`](https://github.com/neurolabusc/dcm_validate) repo (~35 submodules) and should be run before any release.
