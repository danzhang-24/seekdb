#!/usr/bin/env bash
# AMD hipVS contest benchmark driver.
#
# Measures the contest metric: end-to-end build time of a lib=cuvs CAGRA
# (HNSW) index, including the first fixed query, for the frozen V5 baseline
# and for the candidate commit, in the paired order the contest prescribes.
#
#   baseline -> candidate   on dataset A
#   candidate -> baseline   on dataset B
#
# Usage (on the instance):
#   bash run_remote_bench.sh <work_dir> [rounds]
#
# Environment overrides:
#   SEEKDB_HOME   prebuilt seekdb tree in the image (default /seekdb_workspace)
#   HIPVS_ROOT    hipVS prefix (default /opt/hipvs)
#   ROCM_ROOT     ROCm prefix  (default /opt/rocm)
#   JOBS          compile parallelism (default nproc)

set -Eeuo pipefail

WORK=${1:?work dir required}
ROUNDS=${2:-1}
SEEKDB_HOME=${SEEKDB_HOME:-/seekdb_workspace}
HIPVS_ROOT=${HIPVS_ROOT:-/opt/hipvs}
ROCM_ROOT=${ROCM_ROOT:-/opt/rocm}
JOBS=${JOBS:-$(nproc)}

BASELINE_COMMIT=634888d9b037153fa143d2c913ba4b25503e1d62   # amd-cagra-contest-baseline-v5
CANDIDATE_COMMIT=e550df66c67876fe96a0fac98e5956907d7a4041

BASE_URL="https://github.com/danzhang-24/seekdb.git"
CONTEST_URL="https://github.com/zihaomu/seekdb.git"

log() { echo "[$(date +%T)] $*" >&2; }
die() { log "FATAL: $*"; exit 1; }

mkdir -p "$WORK"
cd "$WORK"

# ---------------------------------------------------------------- preflight
log "=== preflight ==="
for p in "$HIPVS_ROOT" "$ROCM_ROOT"; do
  [[ -d "$p" ]] || die "missing $p"
done
command -v cmake >/dev/null || die "cmake missing"
command -v gcc  >/dev/null || die "gcc missing"
command -v mysql >/dev/null || die "mysql client missing"
command -v python3 >/dev/null || die "python3 missing"

GPU_INFO=$(amd-smi static 2>/dev/null | grep -E "MARKET_NAME|TARGET_GRAPHICS_VERSION" || true)
log "GPU: ${GPU_INFO:-unknown}"
ROCM_VER=$(cat "$ROCM_ROOT/.info/version" 2>/dev/null | head -1 || echo unknown)
log "ROCm: $ROCM_VER  cores: $(nproc)  mem: $(free -g | awk '/^Mem/{print $2}')GB"

# ------------------------------------------------------------------ sources
fetch_repo() {
  local dir=$1 commit=$2
  if [[ ! -d "$dir/.git" ]]; then
    log "cloning $dir"
    git clone --quiet "$BASE_URL" "$dir"
    git -C "$dir" remote add contest "$CONTEST_URL" 2>/dev/null || true
  fi
  git -C "$dir" fetch --quiet --all 2>/dev/null || true
  git -C "$dir" checkout --quiet --detach "$commit"
  log "$dir at $(git -C "$dir" rev-parse --short HEAD)"
}

build_seekdb() {
  local dir=$1 tag=$2
  local bdir="$dir/build_release"
  local bridge="$WORK/bridge-$tag/libseekdb_cuvs_bridge.so"
  mkdir -p "$(dirname "$bridge")"

  if [[ ! -f "$bridge" ]]; then
    log "building bridge for $tag"
    ( cd "$dir" && HIPVS_ROOT="$HIPVS_ROOT" ROCM_ROOT="$ROCM_ROOT" \
        bash tools/hipvs_bridge/build.sh "$bridge" ) > "$WORK/bridge-$tag.log" 2>&1 \
      || { tail -40 "$WORK/bridge-$tag.log" >&2; die "bridge build failed ($tag)"; }
  fi

  if [[ ! -x "$bdir/src/observer/seekdb" ]]; then
    log "building seekdb for $tag (jobs=$JOBS)"
    ( cd "$dir" && cmake -S . -B build_release -G "Unix Makefiles" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DOB_ENABLE_UNITY=ON \
        -DOB_USE_LLD=ON \
        -DOB_BUILD_CUVS=ON \
        -DOB_BUILD_CUVS_TRACE=OFF \
        -DCUVS_BRIDGE_LIB="$bridge" \
        && cmake --build build_release --target seekdb -j"$JOBS" ) \
        > "$WORK/build-$tag.log" 2>&1 \
      || { tail -60 "$WORK/build-$tag.log" >&2; die "seekdb build failed ($tag)"; }
  fi
  log "$tag built: $bdir/src/observer/seekdb"
}

# ------------------------------------------------------------------- server
# The image ships a ready-to-use observer launcher; reuse it.
start_observer() {
  local base=$1
  # The image's launcher starts the prebuilt observer from $SEEKDB_HOME/bin.
  # To measure a specific build, point the launcher at that build instead.
  if [[ -x "$SEEKDB_HOME/scripts/start_observer.sh" ]]; then
    "$SEEKDB_HOME/scripts/start_observer.sh" "$base" > "$base/start.log" 2>&1 || true
  else
    die "no start_observer.sh under $SEEKDB_HOME/scripts"
  fi
  for _ in $(seq 1 120); do
    if mysql -h127.0.0.1 -P2881 -uroot -N -s -e "select 1" >/dev/null 2>&1; then
      log "observer ready"; return 0
    fi
    sleep 2
  done
  die "observer did not become ready (see $base/start.log)"
}

stop_observer() {
  local base=$1
  pkill -f "observer.*$base" >/dev/null 2>&1 || true
  mysql -h127.0.0.1 -P2881 -uroot -e "shutdown" >/dev/null 2>&1 || true
  sleep 5
}

# --------------------------------------------------------------- dataset gen
# Deterministic pseudo-random float32 data. The official Hidden250K content is
# not published, so this is a stand-in used to compare the two builds against
# each other on identical input; it is not a reproduction of the official set.
gen_data() {
  local tsv=$1 rows=$2 dim=$3 seed=$4
  [[ -f "$tsv" ]] && { log "dataset exists: $tsv"; return 0; }
  log "generating $tsv ($rows x $dim)"
  python3 "$WORK/gen_vectors.py" --rows "$rows" --dim "$dim" --seed "$seed" --out "$tsv"
}

# --------------------------------------------------------------- the measure
# One measured run: data already loaded, index absent, timer around
# CREATE VECTOR INDEX plus the first fixed query.
measure_once() {
  local bin=$1 tsv=$2 query=$3 dim=$4 label=$5
  local base="$WORK/run/$label"
  rm -rf "$base"; mkdir -p "$base"

  # Start the observer FROM THE BUILD UNDER TEST, so each variant is measured
  # with its own binary rather than the image's prebuilt one.
  # The image ships a ready configuration; reuse it and only override the
  # data directory so each run starts from a clean state.
  local conf="$base/config.conf"
  if [[ -f "$SEEKDB_HOME/conf/config.conf" || -f "$SEEKDB_HOME/config.conf" ]]; then
    local src="$SEEKDB_HOME/conf/config.conf"
    [[ -f "$src" ]] || src="$SEEKDB_HOME/config.conf"
    sed -e "s#^dev_dir.*#dev_dir=$base#" "$src" > "$conf"
  fi
  if [[ -x "$bin" && -f "$conf" ]]; then
    "$bin" -f "$conf" > "$base/start.log" 2>&1 &
    echo $! > "$base/pid"
    for _ in $(seq 1 150); do
      mysql -h127.0.0.1 -P2881 -uroot -N -s -e "select 1" >/dev/null 2>&1 && break
      sleep 2
    done
  else
    # Fall back to the image launcher if the binary layout differs.
    ( cd "$base" && "$SEEKDB_HOME/scripts/start_observer.sh" "$base" > start.log 2>&1 || true )
    for _ in $(seq 1 150); do
      mysql -h127.0.0.1 -P2881 -uroot -N -s -e "select 1" >/dev/null 2>&1 && break
      sleep 2
    done
  fi
  if ! mysql -h127.0.0.1 -P2881 -uroot -N -s -e "select 1" >/dev/null 2>&1; then
    die "observer for $label did not come up (see $base/start.log)"
  fi

  mysql -h127.0.0.1 -P2881 -uroot -e "
    create database if not exists bench;
    use bench;
    drop table if exists items;
    create table items(id bigint primary key, v vector($dim));
    load data local infile '$tsv' into table items
      fields terminated by '\\t' lines terminated by '\\n' (id,v);" >/dev/null

  local rows
  rows=$(mysql -h127.0.0.1 -P2881 -uroot -N -s -e "select count(*) from bench.items")
  log "$label: $rows rows loaded"

  # timer starts here
  local t0 t1
  t0=$(date +%s.%N)
  mysql -h127.0.0.1 -P2881 -uroot -e "
    use bench;
    create vector index idx_items on items(v)
      with (distance=l2, type=hnsw, lib=cuvs, m=16, ef_construction=200, ef_search=64);" >/dev/null
  local ids
  ids=$(mysql -h127.0.0.1 -P2881 -uroot -N -s -e "
    use bench; select id from items order by l2_distance(v,$query) approximate limit 10;")
  t1=$(date +%s.%N)
  # timer stops here

  local n
  n=$(printf '%s\n' "$ids" | grep -c '[^[:space:]]' || true)
  if [[ "$n" -ne 10 ]]; then
    log "WARNING: $label first query returned $n ids (expected 10)"
  fi
  printf '%s %.3f\n' "$label" "$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.3f", b-a}')"

  # keep the ids for the result-integrity check
  printf '%s\n' "$ids" > "$base/first_ids.txt"

  # Stop this observer before the next variant so the GPU memory is released
  # and the next measurement starts from a clean device state.
  mysql -h127.0.0.1 -P2881 -uroot -e "shutdown" >/dev/null 2>&1 || true
  if [[ -f "$base/pid" ]]; then
    kill "$(cat "$base/pid")" >/dev/null 2>&1 || true
  fi
  pkill -f "observer.*$base" >/dev/null 2>&1 || true
  sleep 8
}

# --------------------------------------------------------------------- main
main() {
  log "work dir: $WORK"
  log "baseline: $BASELINE_COMMIT"
  log "candidate: $CANDIDATE_COMMIT"

  # Preflight only: verify the GPU, the toolchain and the observer can start,
  # without paying for two full builds. Run this first.
  if [[ "${PREFLIGHT:-0}" == "1" ]]; then
    log "=== preflight mode ==="
    start_observer "$WORK/run/preflight"
    log "observer is up"
    mysql -h127.0.0.1 -P2881 -uroot -N -s -e "select version()" || true
    log "preflight OK"
    exit 0
  fi

  fetch_repo "$WORK/baseline" "$BASELINE_COMMIT"
  fetch_repo "$WORK/candidate" "$CANDIDATE_COMMIT"
  build_seekdb "$WORK/baseline"  baseline
  build_seekdb "$WORK/candidate" candidate

  # datasets
  gen_data "$WORK/sift250k.tsv"  250000 128 20260901
  gen_data "$WORK/hidden250k.tsv" 250000 256 20260902

  local q128 q256
  q128=$(head -1 "$WORK/sift250k.tsv"   | cut -f2)
  q256=$(head -1 "$WORK/hidden250k.tsv" | cut -f2)

  local B="$WORK/build_release/bin"  C="$WORK/build_release/cand"
  : > "$WORK/results.txt"

  for r in $(seq 1 "$ROUNDS"); do
    # dataset A: baseline -> candidate
    log "round $r  SIFT250K   baseline"
    measure_once "$WORK/baseline/build_release/src/observer/seekdb" \
      "$WORK/sift250k.tsv" "$q128" 128 "sift_baseline_r$r" | tee -a "$WORK/results.txt"
    log "round $r  SIFT250K   candidate"
    measure_once "$WORK/candidate/build_release/src/observer/seekdb" \
      "$WORK/sift250k.tsv" "$q128" 128 "sift_candidate_r$r" | tee -a "$WORK/results.txt"

    # dataset B: candidate -> baseline (order swapped to cancel thermal drift)
    log "round $r  Hidden250K candidate"
    measure_once "$WORK/candidate/build_release/src/observer/seekdb" \
      "$WORK/hidden250k.tsv" "$q256" 256 "hidden_candidate_r$r" | tee -a "$WORK/results.txt"
    log "round $r  Hidden250K baseline"
    measure_once "$WORK/baseline/build_release/src/observer/seekdb" \
      "$WORK/hidden250k.tsv" "$q256" 256 "hidden_baseline_r$r" | tee -a "$WORK/results.txt"
  done

  log "=== raw results ==="
  cat "$WORK/results.txt"

  # speedups
  python3 "$WORK/summarize.py" "$WORK/results.txt" | tee "$WORK/summary.txt"
}

main "$@"
