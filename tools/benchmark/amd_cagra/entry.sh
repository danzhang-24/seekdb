#!/usr/bin/env bash
# One-liner entry point for the AMD hipVS benchmark, meant to be run inside
# the instance (JupyterLab terminal or any shell with network access).
#
# It clones the candidate repository (which carries the benchmark driver under
# tools/benchmark/amd_cagra), then runs the driver. The driver builds the
# frozen V5 baseline and the candidate, generates the datasets, and measures
# them in the paired order the contest prescribes.
#
#   bash /workspace/amd_bench_entry.sh            # full run
#   PREFLIGHT=1 bash /workspace/amd_bench_entry.sh # environment check only
#
# Override the repository with REPO=... and the work dir with WORK=....

set -Eeuo pipefail

REPO=${REPO:-https://github.com/danzhang-24/seekdb.git}
BRANCH=${BRANCH:-amd-cagra-submit-v5}
WORK=${WORK:-/workspace/bench}
SRC=${SRC:-/workspace/seekdb-bench-src}

if [[ ! -d "$SRC/.git" ]]; then
  echo "== cloning $REPO ($BRANCH) =="
  git clone --quiet --depth 1 --branch "$BRANCH" "$REPO" "$SRC"
fi

DRIVER="$SRC/tools/benchmark/amd_cagra/run_remote_bench.sh"
[[ -f "$DRIVER" ]] || { echo "driver not found at $DRIVER" >&2; exit 1; }
[[ -f "$SRC/tools/benchmark/amd_cagra/gen_vectors.py" ]] || { echo "gen_vectors.py missing" >&2; exit 1; }
[[ -f "$SRC/tools/benchmark/amd_cagra/summarize.py" ]] || { echo "summarize.py missing" >&2; exit 1; }

mkdir -p "$WORK"
cp "$SRC/tools/benchmark/amd_cagra/"*.py "$WORK/" 2>/dev/null || true

echo "== running driver (work=$WORK) =="
exec bash "$DRIVER" "$WORK" "${ROUNDS:-1}"
