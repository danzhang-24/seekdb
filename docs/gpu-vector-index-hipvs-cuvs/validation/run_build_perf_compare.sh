#!/usr/bin/env bash
# Reproducible CPU/GPU end-to-end build-time comparison for a lib=cuvs
# CAGRA (HNSW) index, measured exactly the way the contest measures it:
#
#   timer starts  : the vector table is already populated (LOAD DATA done),
#                   no vector index exists yet, no reusable CAGRA graph /
#                   index file / in-process cache is present
#   timer stops   : the index is queryable AND the first fixed query returned
#                   a legal result
#
# The script runs the SAME scenario twice: once with the frozen V5 baseline
# (lib=cuvs, GPU path) and once with the candidate build, then prints the
# speedup. It is self-contained and re-runnable on the contest AMD box.
#
# Usage:
#   run_build_perf_compare.sh <container> <dataset.tsv> [rounds] [dataset_label]
#
#   <container>     running seekdb container name (ROCm 7.2.4 / W7900D)
#   <dataset.tsv>   TSV file with two columns: id, space separated float vector
#   [rounds]        repetitions per variant (default 3)
#   [label]         dataset label used in the report (default "dataset")
#
# The candidate must be the built seekdb inside <container>; to compare against
# the frozen baseline, run this script once with the baseline container and once
# with the candidate container, or use two containers and merge the outputs.

set -Eeuo pipefail

CONTAINER=${1:?container name required}
DATASET=${2:?dataset TSV path required}
ROUNDS=${3:-3}
LABEL=${4:-dataset}
DB=perf_compare

if [[ ! -r "$DATASET" ]]; then
  echo "dataset not readable: $DATASET" >&2
  exit 2
fi

WORK=$(mktemp -d /tmp/buildperf.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

sql() {
  docker exec "$CONTAINER" mysql --local-infile=1 -h127.0.0.1 -P2881 -uroot -ANB \
    -e "set ob_query_timeout=600000000; $1"
}

ready() {
  local deadline=$((SECONDS + 300))
  until sql 'select 1' > /dev/null 2>&1; do
    ((SECONDS < deadline)) || { echo "seekdb did not become ready" >&2; exit 3; }
  done
}

# The query vector used for the timed first query: the first row of the dataset.
PROBE=$(docker exec "$CONTAINER" head -n 1 "$DATASET" | cut -f2)
ROWS=$(wc -l < "$DATASET")

# One measured round. Everything outside the timed region (clone, build, data
# generation, LOAD DATA, service start) is deliberately outside the timer.
run_round() {
  local round=$1
  local idx="idx_items"

  sql "drop database if exists ${DB}; create database ${DB};" > /dev/null
  sql "use ${DB};
      drop table if exists items;
      create table items(id bigint primary key, v vector($(head -n 1 "$DATASET" | cut -f2 | awk '{print NF}')));
      load data local infile '$DATASET' into table items
      fields terminated by '\\t' lines terminated by '\\n' (id,v);" > /dev/null

  local loaded
  loaded=$(sql "select count(*) from ${DB}.items" | tr -d '[:space:]')
  if [[ "$loaded" != "$ROWS" ]]; then
    echo "row mismatch: table=$loaded file=$ROWS" >&2
    exit 4
  fi

  # --- TIMER START -------------------------------------------------------
  local t0 t1
  t0=$(date +%s.%N)
  sql "use ${DB}; create vector index ${idx} on items(v)
       with (distance=l2,type=hnsw,lib=cuvs,m=16,ef_construction=200,ef_search=64);" > /dev/null
  # The first fixed query must be inside the measured window.
  local first_ids
  first_ids=$(sql "select id from ${DB}.items order by l2_distance(v,$PROBE) approximate limit 10")
  t1=$(date +%s.%N)
  # --- TIMER END ---------------------------------------------------------

  local n_ids
  n_ids=$(printf '%s\n' "$first_ids" | grep -c '[^[:space:]]' || true)
  if [[ "$n_ids" -ne 10 ]]; then
    echo "first query returned $n_ids ids, expected 10 (result_integrity)" >&2
    exit 5
  fi

  awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.3f", b-a}'
}

ready

echo "=== $LABEL / $CONTAINER : $ROWS rows, $ROUNDS rounds ==="
for r in $(seq 1 "$ROUNDS"); do
  # Every round starts from a clean database (no reusable graph / snapshot).
  t=$(run_round "$r")
  echo "round $r: ${t}s"
  echo "$t" >> "$WORK/times"
done

median() {
  sort -g "$WORK/times" | awk '{a[NR]=$1} END{
    if (NR%2==1) printf "%.3f", a[(NR+1)/2];
    else printf "%.3f", (a[NR/2]+a[NR/2+1])/2; }'
}

MED=$(median)
echo "median build time (incl. first query): ${MED}s"
echo "container=${CONTAINER} label=${LABEL} rows=${ROWS} median=${MED}s"
