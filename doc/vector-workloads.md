# Vector Similarity Search & Index Aging Benchmark Guide (`pgbench-vec`)

`workloads/pgbench-vec/` implements a comprehensive vector Approximate Nearest Neighbor (ANN), hybrid filtered Retrieval-Augmented Generation (RAG), multi-vector late-interaction, and continuous index aging benchmark suite inspired by the academic **VecBench** methodology.

Unlike traditional database benchmarks that only measure throughput (TPS) and latency (ms), `pgbench-vec` treats **retrieval accuracy (`Recall@K` and `nDCG@K`)** and **index aging degradation (`initial Q1` vs. `mature Q4` recall)** as first-class telemetry metrics with enforceable `--sla` contracts.

> **Disclaimer**: `pgbench-vec` is a **non-comparable, inspired-by engineering workload** for PostgreSQL testing and development. Results represent un-audited internal engineering measurements and do not constitute official VecBench leaderboard ratings.

---

## 1. Key Capabilities & Extension Support

### 1.1 Works With or Without `pgvector` (`CREATE EXTENSION vector`)
`pgbench_vec_schema.sql` automatically detects whether the `vector` extension (`pgvector`) is available on the target PostgreSQL server:
- **When `pgvector` is installed**: Embedding columns use native `vector(dim)` types and `<->` (L2), `<=>` (Cosine), or `<#>` (Inner Product) operators with HNSW or IVFFlat indexes.
- **When `pgvector` is not installed**: `pgbench-vec` seamlessly runs on any stock PostgreSQL 14–20devel instance using `contrib/pgbench`'s fast C `pgbench.vector_distance(v1, v2, 'l2')` function over formatted vector representations.

### 1.2 First-Class Vector Support in `pgbench`
- **Native `PGBT_VECTOR` Script Type**: Client variables store dense float vectors (`[0.12, -0.34, ...]`) and sparse vectors (`{1:0.5, 42:0.8}/1000`), automatically parsed by `\gset` and `\gset_col`.
- **Clustered & Planted Synthetic Embeddings (`contrib/pgbench`)**:
  - `pgbench.clustered_vector(dim, num_clusters, sigma, seed, cluster_id)`: Generates Gaussian clusters around deterministic unit-sphere centroids so ANN indexes experience realistic cluster geometry rather than uniform white-noise hypercubes.
  - `pgbench.planted_vector(dim, target_id, delta, seed)`: Generates query embeddings at controlled perturbation distance `delta` from known target centroids.
- **Single-Column Ranking Capture (`\gset_col`)**:
  - `SELECT id FROM ... ORDER BY ... LIMIT 10 \gset_col retrieved` binds all returned `id` rows directly into a 1D `PGBT_ARRAY` variable `:retrieved` in a single step.
- **Named `Recall@K` Telemetry & Sliding Ring Buffers (`\record_recall`)**:
  - `\record_recall name=r_unfiltered retrieved truth 10` computes $|\text{retrieved}_{1..K} \cap \text{truth}_{1..K}| / \min(K, |\text{truth}|)$, updates a 10-bucket tail distribution (`[0.0-0.1)` .. `[0.9-1.0]`), and feeds a 32-sample sliding ring buffer that compares initial (`Q1`) recall against mature (`Q4`) recall after continuous writes.

---

## 2. The 4-Stage Initialization Pipeline (Including Ground Truth)

Unlike relational suites that use 3 initialization stages, **`pgbench-vec` requires 4 initialization stages** because exact top-$K$ ground-truth neighbor lists must be precomputed into `pgbench_vec_ground_truth` after the vector table is populated.

### 2.1 Single-Command Initialization via `.pgbench.json` (Recommended)

Both `vec_hybrid.pgbench.json` and `vec_aging.pgbench.json` include all 4 stages (`schema` $\rightarrow$ `data` $\rightarrow$ `constraints` $\rightarrow$ `ground_truth`) in their `init_steps` array:

```bash
createdb bench_vec

# Executes all 4 stages including pgbench_vec_gt_gen.sql automatically:
pgbench --config=workloads/pgbench-vec/vec_hybrid.pgbench.json -i -s 1 bench_vec
```

### 2.2 Manual 4-Stage Initialization (`-n -t 1`)

```bash
export W=4
export SCALE=1

# Stage 1: DDL Schema (pgbench_vec_items, pgbench_vec_queries, pgbench_vec_ground_truth)
pgbench -n -t 1 -f workloads/pgbench-vec/pgbench_vec_schema.sql bench_vec

# Stage 2: Parallel Clustered Vector Generation (1,000 items * scale, 100 query vectors)
pgbench -n -c $W -j $W -t 1 -s $SCALE -D scale=$SCALE -D num_workers=$W \
    -f workloads/pgbench-vec/pgbench_vec_init.sql bench_vec

# Stage 3: Primary Keys, Scalar B-Tree Indexes, GIN JSONB/Tag Indexes & ANALYZE
pgbench -n -t 1 -f workloads/pgbench-vec/pgbench_vec_constraints.sql bench_vec

# Stage 4 (MANDATORY): Precompute Exact L2 Top-K Ground Truth (unfiltered, filtered, multivec)
pgbench -n -t 1 -f workloads/pgbench-vec/pgbench_vec_gt_gen.sql bench_vec
```

> **Important**: Never skip Stage 4 (`pgbench_vec_gt_gen.sql`). Without precomputed ground truth in `pgbench_vec_ground_truth`, `\record_recall` will compare retrieved IDs against an empty ground-truth array and report `recall = 0.0`.

---

## 3. The Six Operational Vector Benchmark Modes

`pgbench-vec` provides two ready-to-run `.pgbench.json` suites that cover all **6 VecBench operational modes**:
- **`vec_hybrid.pgbench.json`**: Runs Modes 1–5 concurrently as a weighted workload mix with 4 independent per-mode `Recall@K` SLAs.
- **`vec_aging.pgbench.json`**: Runs Mode 6 (`pgbench_vec_aging.sql`) to measure continuous ingestion, centroid drift, delete/re-insert churn, and `Q1`-vs-`Q4` index recall degradation.

| Mode | Script File | Weight in `vec_hybrid` | Named Recall Stream | Default SLA Target | Description |
| :---: | :--- | :---: | :---: | :---: | :--- |
| **Mode 1** | `pgbench_vec_search.sql` | **30%** | `r_unfiltered` | `recall>=0.95` | **Unfiltered Top-K Search**: Pure ANN similarity search (`ORDER BY distance LIMIT 10`) compared against exact `gt_unfiltered`. |
| **Mode 2** | `pgbench_vec_filtered.sql` | **25%** | `r_prefilter` | `recall>=0.90` | **Pre-Filtered Hybrid Search**: Combines scalar category predicate (`category_id = :q_cat`), JSONB/tag filter, and vector ordering against `gt_filtered`. |
| **Mode 3** | `pgbench_vec_postfiltered.sql` | **20%** | `r_postfilter` | `recall>=0.40` | **Post-Filtered Oversampling Search**: Fetches a $4\times$ oversampled ANN candidate pool (`LIMIT 40`) in a subquery and applies scalar filtering (`WHERE category_id = :q_cat LIMIT 10`) in the outer query. |
| **Mode 4** | `pgbench_vec_insert.sql` | **10%** | *(write stream)* | `p95<=5000ms` | **Streaming Vector Ingestion**: Generates new clustered embeddings, upserts into `pgbench_vec_items`, pushes new IDs into `--reservoir=vec_ids:1024`, and advances causal watermarks (`\advance_watermark`). |
| **Mode 5** | `pgbench_vec_multivec.sql` | **15%** | `r_multivec` | `recall>=0.85` | **Multi-Vector Score Fusion**: Late-interaction weighted distance fusion (`0.7 * text_dist + 0.3 * image_dist`) across `embedding` and `image_embedding` columns compared against `gt_multivec`. |
| **Mode 6** | `pgbench_vec_aging.sql` | **100%** *(in `vec_aging`)* | `r_aging` | `recall>=0.80` | **Continuous Ingestion, Drift, Churn & Aging**: Interleaves streaming vector upserts, embedding perturbation (drift), delete/re-insert churn, live ground-truth refresh, and `Recall@10` probes (`r_aging`). |

---

## 4. Running `pgbench-vec` Workloads

### 4.1 Hybrid Multi-Mode Benchmark (`vec_hybrid.pgbench.json` — Modes 1–5)

```bash
# Run via --config (automatically configures --reservoir=vec_ids:1024 and all 5 scripts + SLAs)
pgbench --config=workloads/pgbench-vec/vec_hybrid.pgbench.json \
    -c 4 -j 2 -T 30 \
    --json-report=report_vec_hybrid.json bench_vec

# Or run with explicit CLI flags:
pgbench -n -c 4 -j 2 -T 30 -s 1 -D scale=1 \
    --reservoir=vec_ids:1024 \
    -f workloads/pgbench-vec/pgbench_vec_search.sql@30 \
    -f workloads/pgbench-vec/pgbench_vec_filtered.sql@25 \
    -f workloads/pgbench-vec/pgbench_vec_postfiltered.sql@20 \
    -f workloads/pgbench-vec/pgbench_vec_multivec.sql@15 \
    -f workloads/pgbench-vec/pgbench_vec_insert.sql@10 \
    --sla="total:r_unfiltered:recall>=0.95" \
    --sla="total:r_prefilter:recall>=0.90" \
    --sla="total:r_postfilter:recall>=0.40" \
    --sla="total:r_multivec:recall>=0.85" \
    --sla="total:p95<=100ms" \
    --json-report=report_vec_hybrid.json bench_vec
```

### 4.2 Index Aging & Degradation Benchmark (`vec_aging.pgbench.json` — Mode 6)

Graph-based (HNSW) and centroid-based (IVFFlat) vector indexes often suffer recall degradation as vectors are continuously inserted, updated (drifted), and deleted (churned). `vec_aging.pgbench.json` measures this phenomenon directly:

```bash
pgbench --config=workloads/pgbench-vec/vec_aging.pgbench.json \
    -c 4 -j 2 -T 60 \
    --sla="total:r_aging:recall>=0.80" \
    --sla="total:p95<=150ms" \
    --log-db=self --run-label="vec-aging-run" \
    --json-report=report_vec_aging.json bench_vec
```

---

## 5. Understanding `Recall@K` & Aging Degradation Telemetry

In both stdout summary output, `--json-report`, and `--log-db` (`pgbench_runs` / `pgbench_run_recalls`), `pgbench` reports:

1. **Summary Statistics**: `samples`, `avg`, `stddev`, `min`, `max`, `p5` (5th percentile tail recall), `p50` (median recall), and `p95`.
2. **10-Bucket Recall Distribution Histogram**:
   Counts how many queries fell into each decile (`0.0-0.1`, `0.1-0.2`, ..., `0.8-0.9`, `0.9-1.0`) so you can spot heavy-tail recall collapse that a simple mean hides.
3. **Q1 vs. Q4 Aging Degradation (`aging_degradation`)**:
   - `initial_recall` ($Q_1$): Mean recall across the earliest quartile of recorded samples in the run.
   - `mature_recall` ($Q_4$): Mean recall across the final quartile of recorded samples in the sliding ring buffer.
   - `delta`: $\text{mature\_recall} - \text{initial\_recall}$.
   - `degradation_pct`: $\max\!\left(0, \frac{\text{initial\_recall} - \text{mature\_recall}}{\text{initial\_recall}} \times 100\right)$.

Example `--json-report` excerpt from a `pgbench-vec` run:

```json
"recall": {
  "samples": 400,
  "avg": 0.9450,
  "min": 0.8000,
  "max": 1.0000,
  "p5": 0.8000,
  "p50": 1.0000,
  "distribution": {
    "0.0-0.1": 0,
    "0.8-0.9": 88,
    "0.9-1.0": 312
  },
  "aging_degradation": {
    "initial_recall": 0.9600,
    "mature_recall": 0.9300,
    "delta": -0.0300,
    "degradation_pct": 3.1250
  }
}
```

---

## 6. Benchmarking External Binary Vector Datasets (`.fvecs`, `.bvecs`, `.ivecs`)

To benchmark standard external ANN datasets (such as SIFT1M, GIST1M, or Deep1B in Texmex `.fvecs` / `.bvecs` / `.ivecs` binary format), pass `--vector-dataset`, `--vector-base`, and `--ground-truth-table`:

```bash
pgbench -n -c 8 -j 4 -T 60 \
    --vector-base="base:/data/sift/sift_base.fvecs" \
    --vector-dataset="queries:/data/sift/sift_query.fvecs:0based" \
    --ground-truth-table="gt:/data/sift/sift_groundtruth.ivecs" \
    -f custom_sift_search.sql \
    --sla="total:recall>=0.95" \
    bench_vec
```

Inside `custom_sift_search.sql`:
```sql
\set qid random(0, dataset_count('queries') - 1)
\dataset_bind queries AT :qid q_vec
\dataset_bind gt AT :qid gt_ids
SELECT id FROM sift_items ORDER BY embedding <-> :q_vec::vector LIMIT 10 \gset_col ret_ids
\record_recall name=sift_r10 ret_ids gt_ids 10
```
