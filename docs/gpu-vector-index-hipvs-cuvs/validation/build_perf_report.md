# CPU/GPU build-time performance comparison — `lib=cuvs` CAGRA (HNSW)

This document is the reproducible performance report required by the
`seekdb × AMD hipVS` submission: the same index build is measured on the CPU
path (`lib=vsag`) and on the GPU path (`lib=cuvs`), and the candidate build is
compared against the frozen V5 baseline.

- Baseline under comparison: tag `amd-cagra-contest-baseline-v5`,
  commit `634888d9b037153fa143d2c913ba4b25503e1d62`.
- Candidate: the `amd-cagra-submit-v5` branch of this repository.
- Reference machine (contest evaluation environment): AMD Radeon PRO W7900D,
  ROCm 7.2.4, 24 CPU cores, 45 GB VRAM budget.

## 1. What is measured

The measured region follows the contest definition exactly:

| Phase | Inside the timer? |
| --- | --- |
| `git clone`, source build, dataset generation | no |
| `LOAD DATA` into the vector table | no |
| `CREATE VECTOR INDEX ... lib=cuvs` | **yes** |
| data scan + host→device transfer | **yes** |
| CAGRA graph construction + GPU sync | **yes** |
| the first fixed query, returning a legal result | **yes** |

The SQL under test is fixed by the evaluation and is not modified here:

```sql
CREATE VECTOR INDEX idx_items ON items(v)
  WITH (distance=l2, type=hnsw, lib=cuvs, m=16, ef_construction=200, ef_search=64);

SELECT id FROM items ORDER BY l2_distance(v, <query>) APPROXIMATE LIMIT <k>;
```

Each round starts from a clean database, so no CAGRA graph, index file, or
in-process cache from a previous round can be reused.

## 2. How to reproduce

```bash
# 1. Build the hipVS bridge (from this repository).
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
  tools/hipvs_bridge/build.sh /work/bridge/libseekdb_cuvs_bridge.so

# 2. Build seekdb with the GPU backend enabled.
cmake -S . -B build_release -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOB_ENABLE_UNITY=ON \
  -DOB_USE_LLD=ON \
  -DOB_BUILD_CUVS=ON \
  -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB=/work/bridge/libseekdb_cuvs_bridge.so
cmake --build build_release --target seekdb

# 3. Measure. Run against the baseline container and the candidate container,
#    then compare the two medians.
docs/gpu-vector-index-hipvs-cuvs/validation/run_build_perf_compare.sh \
  <container> <dataset.tsv> 3 SIFT250K
```

The runner writes one line per round and a median line, and it fails loudly if
the first query does not return exactly `k` distinct ids (the
`result_integrity` hard gate).

## 3. Baseline reference numbers

These are the frozen-baseline figures published with the V5 baseline, measured
on the reference machine. They are the comparison point, not a claim about the
candidate.

| Dataset | Build time (incl. first query) | Recall@10 / @100 | nq=4096 QPS (Top-10 / Top-100) |
| --- | ---: | ---: | ---: |
| SIFT250K | 20–27 s (median ≈ 23 s) | ≈ 1.000 / 1.000 | ≈ 147 / 143 |
| Hidden250K | 29–35 s (median ≈ 30 s) | ≈ 0.985 / 0.914 | ≈ 87 / 85 |

Absolute times differ per machine; the contest always compares a candidate
against the frozen baseline **in the same round**, so only the ratio matters.

## 4. Candidate result

> **Status: to be filled on the AMD reference machine.**
>
> The development sandbox used for this change has no AMD GPU, no ROCm and no
> hipVS runtime (`/opt/rocm` and `/opt/hipvs` are absent), so the numbers below
> could not be produced locally and are deliberately left empty rather than
> estimated. Fill them in by running the script in section 2 on the reference
> machine and attaching the raw per-round output.

| Dataset | Baseline median | Candidate median | Build speedup |
| --- | ---: | ---: | ---: |
| SIFT250K | 23 s | _TBD_ | _TBD_ |
| Hidden250K | 30 s | _TBD_ | _TBD_ |

| Dataset | Recall@10 | Recall@100 | Query QPS ratio (Top-10 / Top-100) |
| --- | ---: | ---: | ---: |
| SIFT250K | _TBD_ | _TBD_ | _TBD_ |
| Hidden250K | _TBD_ | _TBD_ | _TBD_ |

Hard gates that must stay green: `recall@10_minimum`, `recall@100_minimum`,
`recall@10_baseline_delta`, `recall@100_baseline_delta` (≤ 0.005 drop),
`query_regression` (≥ 0.90 of baseline QPS), `gpu_path`, `result_integrity`,
and the VRAM / host-memory limits.

## 5. What this change does

The candidate keeps the whole V5 GPU path intact and removes one CPU-side
cost from the measured window.

### The cost that is removed

`cal_distance_by_id` — reached from the query path via
`vsag_query_vids` → `query_brute_force_distances` — unconditionally called
`ensure_vsag_fallback()` before computing a distance. On a `lib=cuvs` index
that fallback builds a **complete CPU VSAG HNSW graph** over the whole dataset,
which is a sequential per-row graph insertion. On a 250K-row dataset this is
the dominant CPU cost of the measured window, and it is pure overhead: the GPU
CAGRA graph is the index that answers the query.

### The change

`cal_distance_by_id` now answers a `lib=cuvs` L2 request directly from the
flat row-major buffer that the index already maintains (`buf_vecs_` /
`buf_ids_`), using a lazily built `vid → row` index:

- the exact distance is computed from the buffered vector, so **no CPU VSAG
  graph is built at all**;
- the distance semantics are identical to VSAG's: `l2` is the squared L2
  distance (VSAG returns squared values for `l2`), and a missing id yields
  `-1.0f`, exactly like `CalcDistanceById`;
- the id space is the same one `ensure_vsag_fallback()` would have used to
  build the VSAG graph (`buf_ids_`), so results are unchanged;
- the buffer is restored on restart by the existing GPU snapshot path
  (`ob_cuvs_deserialize`), so the optimization also holds after a restart.

### What is deliberately *not* changed

- The GPU ANN path (`ob_cuvs_try_search`) is untouched: the GPU still builds
  and serves the CAGRA graph, so the `gpu_path` hard gate is unaffected.
- The `ensure_vsag_fallback()` calls in `knn_search` and `fserialize` are kept.
  They are the CPU safety net for the case where the GPU cannot serve a
  request (filter, delete, stale graph, unavailable GPU); removing them would
  turn a GPU failure into an empty result.
- `lib=vsag` indexes are untouched and keep their existing CPU behavior.

## 6. Reproducing the correctness side

The GPU smoke runner covers the GPU path, the CPU fallback and the isolation
between them:

```bash
LD_LIBRARY_PATH=/work/bridge:/opt/hipvs/lib:/opt/rocm/lib \
  docs/gpu-vector-index-hipvs-cuvs/validation/run_gpu_smoke.sh \
    --observer /path/to/build_release/src/observer/seekdb \
    --bridge /work/bridge/libseekdb_cuvs_bridge.so \
    --base-dir /tmp/seekdb-cuvs-smoke \
    --port 2981 \
    --render /dev/dri/renderD135 \
    --evidence /tmp/seekdb-cuvs-evidence
```

The DDL persistence path (build → snapshot → restart → query) is covered by:

```bash
docs/gpu-vector-index-hipvs-cuvs/validation/run_ddl_persistence_smoke.sh <container>
```

The no-GPU DDL contract remains covered by
`tools/deploy/mysql_test/test_suite/vector_index/t/vector_index_cuvs_ddl.test`.
