#!/usr/bin/env bash
# Ablation matrix for the full-text index performance task.
#
# Builds each branch in turn, runs the official benchmark against it, scores
# the report with the checked-in scoring script, and prints one table with the
# per-category improvement and the resulting score.
#
# The benchmark and the scoring script are the ones the CI uses; this driver
# only automates the build/run/score loop and the comparison.
#
# Usage:
#   tools/benchmark/fts_ablation_matrix.sh [variant ...]
#
# With no arguments every known variant is run. A variant is either a branch
# name in this repository or a path to a build directory.
#
# Environment:
#   BUILD_ROOT   where the per-variant build trees go (default /tmp/fts-ablation)
#   OUT_DIR      where the reports are written       (default $BUILD_ROOT/reports)
#   JOBS         compile parallelism                  (default nproc)
#   SKIP_BUILD   set to 1 to reuse existing build dirs
#   MYSQL        mysql client command
#   MYSQL_VERBOSE mysql client command for the loader

set -Eeuo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BENCH="${REPO_ROOT}/tools/benchmark/fts_large_bench.sh"
SCORE="${REPO_ROOT}/tools/benchmark/fts_large_bench_score.py"
BUILD_ROOT="${BUILD_ROOT:-/tmp/fts-ablation}"
OUT_DIR="${OUT_DIR:-${BUILD_ROOT}/reports}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

export MYSQL="${MYSQL:-mysql -h127.0.0.1 -P2881 -uroot -N -s --default-character-set=utf8mb4}"
export MYSQL_VERBOSE="${MYSQL_VERBOSE:-mysql -h127.0.0.1 -P2881 -uroot --default-character-set=utf8mb4}"

# Variants, in the order they should be measured. "base" is the unmodified
# 2026-competition branch and acts as the reference point for the ablation;
# the official score always compares against the checked-in CI baseline, not
# against this reference.
VARIANTS=(
  "base:2026-competition"
  "known-good:perf/fts-known-good-12.637"
  "candidate:perf/fts-candidate"
  "newsort-only:perf/fts-newsort-only"
  "parallel4-only:perf/fts-parallel4-only"
  "queryonly-known-good:perf/fts-queryonly-known-good"
  "prefixclone:perf/fts-prefixclone"
  "tokenize-parser-props-cache:perf/fts-tokenize-parser-props-cache"
)

if [[ $# -gt 0 ]]; then
  SELECTED=()
  for want in "$@"; do
    for v in "${VARIANTS[@]}"; do
      [[ "${v%%:*}" == "${want}" ]] && SELECTED+=("${v}")
    done
  done
  if [[ ${#SELECTED[@]} -eq 0 ]]; then
    echo "ERROR: no known variant matched: $*" >&2
    echo "known variants: ${VARIANTS[*]%%:*}" >&2
    exit 2
  fi
  VARIANTS=("${SELECTED[@]}")
fi

mkdir -p "${BUILD_ROOT}" "${OUT_DIR}"

log() { echo "$@" >&2; }

# Build one variant into its own build directory. The observer binary is the
# only artifact the benchmark needs.
build_variant() {
  local name="$1" ref="$2"
  local dir="${BUILD_ROOT}/${name}"
  if [[ "${SKIP_BUILD:-0}" == "1" && -x "${dir}/build_release/src/observer/seekdb" ]]; then
    log "  reuse build: ${dir}"
    return 0
  fi
  log "  building ${name} from ${ref} ..."
  rm -rf "${dir}"
  git -C "${REPO_ROOT}" worktree add --detach "${dir}" "${ref}" >/dev/null 2>&1 || {
    git -C "${REPO_ROOT}" worktree prune
    git -C "${REPO_ROOT}" worktree add --detach "${dir}" "${ref}" >/dev/null
  }
  # The worktree shares the repository, so the benchmark script and the scoring
  # script are already present at their expected paths.
  (
    cd "${dir}"
    ./build.sh release --make -j"${JOBS}" > "${dir}/build.log" 2>&1
  ) || {
    log "  BUILD FAILED for ${name}, see ${dir}/build.log"
    tail -30 "${dir}/build.log" >&2 || true
    return 1
  }
  log "  built ${name}"
}

# Run the official benchmark and score it. The benchmark drops and recreates
# its own database, so it must run against a dedicated instance.
run_variant() {
  local name="$1" dir="$2"
  local report="${OUT_DIR}/${name}.report"
  local scored="${OUT_DIR}/${name}.score"
  rm -f "${report}" "${scored}"
  log "  benchmarking ${name} ..."
  (
    cd "${dir}"
    LABEL="${name}" ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 \
      SAMPLES=3 WARMUP=30 SKIP_LOAD=0 OUTPUT="${report}" \
      bash tools/benchmark/fts_large_bench.sh
  ) > "${OUT_DIR}/${name}.bench.log" 2>&1 || {
    log "  BENCHMARK FAILED for ${name}, see ${OUT_DIR}/${name}.bench.log"
    tail -30 "${OUT_DIR}/${name}.bench.log" >&2 || true
    return 1
  }
  python3 "${SCORE}" "${report}" > "${scored}" 2> "${OUT_DIR}/${name}.score.err" || {
    log "  SCORING FAILED for ${name}"
    cat "${OUT_DIR}/${name}.score.err" >&2 || true
    return 1
  }
  log "  scored ${name}: $(grep -m1 '^score:' "${scored}" || echo '?')"
}

main() {
  log "repo:        ${REPO_ROOT}"
  log "build root:  ${BUILD_ROOT}"
  log "reports:     ${OUT_DIR}"
  log "jobs:        ${JOBS}"
  log ""

  local failed=()
  for v in "${VARIANTS[@]}"; do
    local name="${v%%:*}"
    local ref="${v#*:}"
    log "=== ${name} (${ref}) ==="
    if build_variant "${name}" "${ref}"; then
      run_variant "${name}" "${BUILD_ROOT}/${name}" || failed+=("${name}")
    else
      failed+=("${name}")
    fi
    log ""
  done

  log "=== summary ==="
  printf "%-30s %10s %12s %12s %12s\n" "variant" "score" "build" "tokenize" "query"
  printf "%-30s %10s %12s %12s %12s\n" "------------------------------" "----------" \
    "------------" "------------" "------------"
  for v in "${VARIANTS[@]}"; do
    local name="${v%%:*}"
    local scored="${OUT_DIR}/${name}.score"
    if [[ -f "${scored}" ]]; then
      local s b t q
      s="$(grep -m1 '^score:' "${scored}" | awk '{print $2}')"
      b="$(grep -m1 '^build_improvement:' "${scored}" | awk '{print $2}')"
      t="$(grep -m1 '^tokenize_improvement:' "${scored}" | awk '{print $2}')"
      q="$(grep -m1 '^query_improvement:' "${scored}" | awk '{print $2}')"
      printf "%-30s %10s %12s %12s %12s\n" "${name}" "${s:-?}" "${b:-?}" "${t:-?}" "${q:-?}"
    else
      printf "%-30s %10s\n" "${name}" "FAILED"
    fi
  done

  if [[ ${#failed[@]} -gt 0 ]]; then
    log ""
    log "failed variants: ${failed[*]}"
    exit 1
  fi
  log ""
  log "per-variant reports: ${OUT_DIR}/<variant>.report"
  log "per-variant scores : ${OUT_DIR}/<variant>.score"
}

main "$@"
