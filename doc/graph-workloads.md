<!--
  INSPIRATION & TRADEMARK DISCLAIMER:
  The pgbench-graph* workload suites in this repository are clean-room, non-comparable engineering
  workloads historically inspired by public specifications from the Graph Data Council (LDBC LSQB,
  SNB Interactive, SNB BI, FinBench, and Graphalytics). Those names are mentioned in this opening
  header comment to acknowledge architectural inspiration; results represent un-audited internal
  engineering measurements and do not constitute official certified council benchmark results.
-->
# Graph Database Benchmark Suites Guide (`pgbench-graph*`)

`pgbench4perffarm` includes a comprehensive property graph database benchmarking ecosystem covering both **social network property graphs** and **financial transaction graphs** across **4 workload suites** and **multiple SQL/PGQ execution engines**:

1. **`workloads/pgbench-graph/` (`SQL`)**: Standard relational SQL joins and `WITH RECURSIVE ... UNION ALL` CTEs with `ARRAY[...]` path tracking (runs on any PostgreSQL 14–20devel server).
2. **`workloads/pgbench-graph-sql-ud/` (`SQL_UD`)**: Recursive CTEs utilizing `WITH RECURSIVE ... UNION DISTINCT ON (vid ORDER BY weight ASC)` for inline shortest-path, BFS, and connected-components frontier pruning.
3. **`workloads/pgbench-graph-pgq/` (`PGQ`)**: Native ISO/IEC 9075-16 `SQL/PGQ` declarative property graph DDL (`CREATE PROPERTY GRAPH snb_graph` / `finbench_graph`) and `GRAPH_TABLE (... MATCH ... COLUMNS (...))` pattern matching.
4. **Optional `plPGQ` Mode**: Procedural/macro `GRAPH_TABLE(snb_graph(), 'MATCH ...')` via the `pl_pgq` extension over `plv8` or `pljs`.

> **Disclaimer**: All `pgbench-graph*` suites are **non-comparable, clean-room engineering workloads** (`LSQB`, `SNB Interactive`, `SNB BI`, `FinBench`, and `Graphalytics`). Results represent un-audited internal engineering measurements and do not constitute official certified council benchmark results.

---

## 1. Unified Graph Schema & Correlated Generators

### 1.1 The 15-Table `BIGINT`-Aligned Schema (`pgbench_graph_schema.sql`)

All graph suites share a unified relational schema consisting of **10 Social Network (SNB) tables** and **5 Financial Graph (FinBench) tables**. Every primary key (`id`) and foreign key across all vertex and edge tables is typed as **`bigint`** so that ISO `SQL/PGQ` `CREATE PROPERTY GRAPH ... PROPERTIES ALL COLUMNS` requires zero type casts across shared vertex labels:

| Domain | Vertex Tables | Edge / Relationship Tables |
| :--- | :--- | :--- |
| **Social Network (`snb_graph` — 10 tables)** | `pgbench_graph_places` (`id`, `name`, `type`, `part_of_id`)<br>`pgbench_graph_tags` (`id`, `name`, `tagclass_id`)<br>`pgbench_graph_persons` (`id`, `first_name`, `last_name`, `gender`, `city_id`, `company_id`, `university_id`, `created_ts`)<br>`pgbench_graph_forums` (`id`, `title`, `moderator_id`, `created_ts`)<br>`pgbench_graph_messages` (`id`, `creator_id`, `forum_id`, `parent_message_id`, `country_id`, `content`, `length`, `created_ts`) | `pgbench_graph_knows` (`src_id`, `dst_id`, `weight`, `created_ts`)<br>`pgbench_graph_likes` (`person_id`, `message_id`, `created_ts`)<br>`pgbench_graph_person_tags` (`person_id`, `tag_id`)<br>`pgbench_graph_message_tags` (`message_id`, `tag_id`)<br>`pgbench_graph_forum_members` (`forum_id`, `person_id`, `joined_ts`) |
| **Financial (`finbench_graph` — 5 tables)** | `pgbench_fin_accounts` (`id`, `owner_id`, `owner_type`, `is_blocked`, `balance`, `created_ts`)<br>`pgbench_fin_loans` (`id`, `borrower_id`, `acct_id`, `loan_amount`, `balance`, `created_ts`)<br>`pgbench_fin_mediums` (`id`, `type`, `is_blocked`, `risk_score`) | `pgbench_fin_transfers` (`id`, `src_acct`, `dst_acct`, `amount`, `created_ts`)<br>`pgbench_fin_sign_ins` (`medium_id`, `acct_id`, `created_ts`) |

### 1.2 Correlated Power-Law & Flashmob Generators (`contrib/pgbench`)

Real social and financial graphs are not uniform random graphs—they exhibit strong community homophily (friends of friends likely belong to the same community) and bursty temporal "flashmob" spikes. `<suite>_init.sql` generates realistic graph topology in parallel inside PostgreSQL using two specialized C functions in `contrib/pgbench`:

- **`pgbench.ldbc_correlated_edge(src_id, num_vertices, num_communities DEFAULT 32, homophily_prob DEFAULT 0.70, seed DEFAULT 0)`**:
  Generates target vertex IDs where `70%` of edges remain within `src_id`'s local community cluster and `30%` form power-law cross-community bridges.
- **`pgbench.ldbc_flashmob_ts(base_ts_us, span_us, num_spikes DEFAULT 16, spike_prob DEFAULT 0.35, seed DEFAULT 0)`**:
  Generates microsecond timestamps over `[base_ts_us, base_ts_us + span_us)` where `35%` of events cluster tightly around `16` deterministic flashmob event spikes.

---

## 2. Initialization Pipeline (`SQL`, `SQL_UD`, and `PGQ`)

### 2.1 Single-Command Initialization via `.pgbench.json`

Each graph workload directory (`workloads/pgbench-graph/`, `workloads/pgbench-graph-sql-ud/`, `workloads/pgbench-graph-pgq/`) includes 5 ready-to-run `.pgbench.json` files (`lsqb.pgbench.json`, `snb_interactive.pgbench.json`, `snb_bi.pgbench.json`, `finbench.pgbench.json`, `graphalytics.pgbench.json`):

```bash
createdb bench_graph

# Initialize schema, parallel correlated graph data, edge deduplication, PKs/indexes, and ANALYZE:
pgbench --config=workloads/pgbench-graph/lsqb.pgbench.json -i -s 1 bench_graph
```

### 2.2 Manual 3-Stage Initialization (`-n -t 1`)

```bash
export W=4
export SCALE=1

# Stage 1: DDL Schema & Stored Procedures (-n -t 1)
pgbench -n -t 1 -f workloads/pgbench-graph/pgbench_graph_schema.sql bench_graph

# Stage 2: Parallel Correlated Graph Data Population across W workers
pgbench -n -c $W -j $W -t 1 -s $SCALE -D scale=$SCALE -D num_workers=$W \
    -f workloads/pgbench-graph/pgbench_graph_init.sql bench_graph

# Stage 3: Bidirectional Edge Deduplication, Primary Keys, Adjacency Indexes & ANALYZE (-n -t 1)
pgbench -n -t 1 -f workloads/pgbench-graph/pgbench_graph_constraints.sql bench_graph

# Stage 3b (Optional): Layer ISO SQL/PGQ property graph definitions onto an existing pgbench-graph DB
pgbench -n -t 1 -f workloads/pgbench-graph/pgbench_graph_pgq_ddl.sql bench_graph
```

### 2.3 Template Database Cloning for Multi-Engine Comparisons

When comparing `SQL`, `SQL_UD`, and `PGQ` side-by-side, initialize a single master database (`pgbench_graph_template`) once and clone per-test databases via `CREATE DATABASE ... TEMPLATE ...` so every engine runs against identical initial vertices, edges, and optimizer statistics:

```bash
createdb pgbench_graph_template
pgbench --config=workloads/pgbench-graph/lsqb.pgbench.json -i -s 1 pgbench_graph_template
pgbench -n -t 1 -f workloads/pgbench-graph/pgbench_graph_pgq_ddl.sql pgbench_graph_template

# Clone isolated databases for each suite
for mode in sql sql_ud pgq; do
  for suite in lsqb snb_int snb_bi finbench graphalytics; do
    psql -d postgres -c "CREATE DATABASE graph_${mode}_${suite} TEMPLATE pgbench_graph_template;"
  done
done
```

---

## 3. Detailed Guide to the Four Graph Workload Suites

### Suite 1: Labelled Subgraph Query Benchmark (`lsqb.pgbench.json`)

**Purpose**: Stress-tests the query optimizer's join ordering, cardinality estimation, cyclic triangle closure, star joins, and anti-join (`NOT EXISTS`) execution across 9 structural subgraph patterns (`Q1` through `Q9`).

- **Queries Included**:
  - `Q1`–`Q3`: Long linear chain and cyclic triangle patterns (`Person -[KNOWS]-> Person -[KNOWS]-> Person` within the same `Country` / `City`).
  - `Q4`–`Q6`: Star-shaped message/reply/like/tag neighborhood expansions and 2-hop `KNOWS` paths.
  - `Q7`–`Q9`: Optional outer joins and anti-joins (`Person1 -[KNOWS]-> Person2 -[KNOWS]-> Person3` where `NOT (Person1 -[KNOWS]-> Person3)` and shared `Tag`).
- **In-Script Correctness Validation**: Every query asserts structural invariants via `\validate_result (:qN_count >= 0) true` (9 validations per transaction), enforced via `--sla="validation_failures<=0"`.

```bash
# Run LSQB (SQL mode)
pgbench --config=workloads/pgbench-graph/lsqb.pgbench.json \
    -c 4 -j 4 -t 5 --random-seed=12345 \
    --json-report=report_lsqb_sql.json bench_graph

# Run LSQB (ISO SQL/PGQ GRAPH_TABLE mode)
pgbench --config=workloads/pgbench-graph-pgq/lsqb.pgbench.json \
    -c 4 -j 4 -t 5 --random-seed=12345 \
    --json-report=report_lsqb_pgq.json bench_graph
```

---

### Suite 2: Social Network Interactive v1 & v2 (`snb_interactive.pgbench.json`)

**Purpose**: Simulates a live, high-concurrency social network with simultaneous read and write client groups, lock-free entity reservoirs, subroutine calls (`\call`), causal consistency watermarks, and cascading deletes.

- **Key `pgbench` Features Exercised**:
  1. **Dual Client Groups (`--group`)**:
     - `snb_readers`: Executes Complex Reads (`pgbench_ldbc_*_snb_ic.sql` @ `30%`) and Short Reads (`pgbench_ldbc_*_snb_is.sql` @ `70%`).
     - `snb_writers`: Executes Transactional Inserts (`pgbench_ldbc_*_snb_iu.sql` @ `75%`) and v2 Cascading Deletes (`pgbench_ldbc_*_snb_id.sql` @ `25%`).
  2. **Shared Lock-Free Reservoirs (`--reservoir=persons:4096 --reservoir=messages:8192`)**:
     - Writers (`IU1`–`IU8`) push newly inserted person and message IDs into shared atomic ring buffers via `\reservoir_push persons :new_pid` and `\reservoir_push messages :new_mid`.
     - Readers (`IS`/`IC`) and Deleters (`ID`) uniformly sample live IDs via `\reservoir_sample persons p_id :fallback_pid` without extra database lookups.
  3. **Subroutine Calls (`\call`)**:
     - After executing a Complex Read (`IC1`–`IC13`), `pgbench_ldbc_*_snb_ic.sql` invokes `\call pgbench_ldbc_*_snb_is.sql p_id=:p_id` to execute follow-up Short Reads (`IS1`–`IS7`) on the discovered vertices.
  4. **Contiguous Min-Watermark Tracking (`\begin_dependent_write` / `\end_dependent_write` / `\wait_until`)**:
     - Ensures readers never observe dangling references to in-flight concurrent writes.

```bash
# Run SNB Interactive v1/v2 via --config
pgbench --config=workloads/pgbench-graph/snb_interactive.pgbench.json \
    -T 30 --random-seed=12345 \
    --json-report=report_snb_int.json bench_graph

# Or run with explicit CLI flags:
pgbench -n -T 30 -s 1 --tcr=0.1 --random-seed=12345 \
    --reservoir=persons:4096 --reservoir=messages:8192 \
    --group="name=snb_readers,clients=4,threads=2,file=workloads/pgbench-graph/pgbench_ldbc_snb_ic.sql@30,file=workloads/pgbench-graph/pgbench_ldbc_snb_is.sql@70" \
    --group="name=snb_writers,clients=2,threads=2,file=workloads/pgbench-graph/pgbench_ldbc_snb_iu.sql@75,file=workloads/pgbench-graph/pgbench_ldbc_snb_id.sql@25" \
    --sla="validation_failures<=0" \
    --json-report=report_snb_int.json bench_graph
```

> **Note on Standalone `IC` Runs**: Because `pgbench_ldbc_*_snb_ic.sql` invokes `\call pgbench_ldbc_*_snb_is.sql`, always include `-f .../pgbench_ldbc_*_snb_is.sql@0` (weight `0`) if you run `snb_ic.sql` outside `--config=snb_interactive.pgbench.json`.

---

### Suite 3: Social Network Business Intelligence (`snb_bi.pgbench.json`)

**Purpose**: Executes heavy analytical graph aggregations (`BI1` posting summary, `BI2` tag evolution, `BI3` country message cascades, `BI4` top message creators in forums, `BI11` friend triangles, `BI19` weighted shortest paths, `BI20` recruitment paths) across three strictly synchronized benchmark phases using `\phase_barrier`.

- **Three-Phase Execution Structure (`pgbench_ldbc_*_snb_bi.sql`)**:
  1. **Phase 1 — Power Run**: Clients execute analytical BI queries over the initial graph state.
  2. **`\phase_barrier power_complete`**: All clients across all worker threads synchronize at the barrier before any client starts Phase 2.
  3. **Phase 2 — Micro-Batch Refresh (`\if :client_id = 0`)**: Client `0` applies a batch of new social network vertices, messages, and `KNOWS` edges and runs `ANALYZE`.
  4. **`\phase_barrier batch_update_complete`**: All clients wait until the micro-batch update commits.
  5. **Phase 3 — Throughput Run**: All clients execute concurrent BI analytical queries over the updated graph and validate post-update invariants via `\validate_result`.

> **Barrier Invariant**: Always run `snb_bi` with a bounded transaction count (`-t <N>`, e.g., `-t 1` or `-t 2`), **not** a wall-clock timer (`-T`), so all clients execute the exact same number of `\phase_barrier` synchronization points.

```bash
pgbench --config=workloads/pgbench-graph/snb_bi.pgbench.json \
    -c 4 -j 4 -t 2 --random-seed=12345 \
    --json-report=report_snb_bi.json bench_graph
```

---

### Suite 4: Financial Graph (`finbench.pgbench.json`) & Graphalytics (`graphalytics.pgbench.json`)

#### 4A. Financial Fraud & Transaction Graph (`pgbench_ldbc_*_finbench.sql`)
Models high-frequency financial accounts, loans, guarantees, and money transfers across four query classes:
- **Simple Reads (`TSR1`–`TSR6`)**: Account balance lookups, recent incoming/outgoing transfer summaries, and blocked-account checks.
- **Complex Reads (`TCR1`–`TCR11`)**: Bounded multi-hop transfer chains (`1..3` hops with strictly increasing timestamps `t1.ts < t2.ts`), circular money-laundering cycle detection (`Account A -> B -> C -> A`), loan repayment ratios, and guarantee-chain risk propagation.
- **Simple Writes (`TW`)**: Account creation, loan issuance, and transfer insertion.
- **Read-Write Fraud Prevention Transactions (`TRW1`–`TRW3`)**: Inside a `\try ... \catch ... \endtry` block, opens a transaction (`BEGIN`), inserts a transfer between two accounts, checks whether the transfer completes a blocked account cycle or exceeds risk thresholds, and **conditionally executes `ROLLBACK` via `RAISE EXCEPTION`** when a rule is violated (recorded cleanly in `expected_rollbacks`).

```bash
pgbench --config=workloads/pgbench-graph/finbench.pgbench.json \
    -c 4 -j 4 -t 10 --random-seed=12345 \
    --json-report=report_finbench.json bench_graph
```

#### 4B. Core Graph Algorithms (`pgbench_ldbc_*_graphalytics.sql`)
Executes five classical graph algorithms directly in SQL (or `SQL_UD` / `SQL/PGQ`) with `\validate_result` correctness assertions on every run:
1. **Breadth-First Search (`BFS`)**: Computes shortest hop depth (`depth 0..4`) from a source person vertex across `pgbench_graph_knows`.
2. **Weakly Connected Components (`WCC`)**: Propagates minimum vertex component labels (`min(comp_id)`) across the undirected social graph.
3. **Single-Source Shortest Paths (`SSSP`)**: Computes minimum cumulative edge weight (`sum(weight)`) paths from a source vertex (in `SQL_UD` mode, uses `UNION DISTINCT ON (dst_id ORDER BY dist ASC)` to prune suboptimal paths in-place inside `RecursiveUnion`).
4. **PageRank (`PR`)**: Iterative damping-factor (`0.85` / `0.15`) rank propagation across vertex out-degrees.
5. **Local Clustering Coefficient (`LCC`)**: Counts closed triangles among each vertex's 1-hop neighbors divided by `d_i * (d_i - 1)` (where `d_i` is the degree of vertex `i`).

```bash
# Compare standard SQL vs. UNION DISTINCT ON (SQL_UD) vs. SQL/PGQ on Graphalytics:
pgbench --config=workloads/pgbench-graph/graphalytics.pgbench.json \
    -c 4 -j 4 -t 10 --random-seed=12345 --json-report=report_alg_sql.json bench_graph

pgbench --config=workloads/pgbench-graph-sql-ud/graphalytics.pgbench.json \
    -c 4 -j 4 -t 10 --random-seed=12345 --json-report=report_alg_sql_ud.json bench_graph

pgbench --config=workloads/pgbench-graph-pgq/graphalytics.pgbench.json \
    -c 4 -j 4 -t 10 --random-seed=12345 --json-report=report_alg_pgq.json bench_graph
```

---

## 4. Micro-Benchmark Traversal Scripts (`1hop`, `2hop`, `add_edge`)

In addition to the 4 full LDBC-inspired suites, each `workloads/pgbench-graph*` directory includes 3 focused micro-benchmark scripts for rapid query planner and index testing:
- `pgbench_graph_1hop.sql`: 1-hop friend neighborhood + recent messages lookup.
- `pgbench_graph_2hop.sql`: 2-hop friend-of-friend recommendation scoring query.
- `pgbench_graph_add_edge.sql`: High-concurrency transactional edge + message insert (`CALL pgbench_graph_add_edge(...)`).
