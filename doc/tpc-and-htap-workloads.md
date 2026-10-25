# Relational OLTP, Decision Support & HTAP Workloads Guide

This guide covers the six relational OLTP, Decision Support (OLAP), and Hybrid Transactional/Analytical Processing (HTAP) workload suites bundled in `workloads/`:

1. **`pgbench-b`** (`workloads/pgbench-b/`): Modernized stored-procedure banking OLTP workload.
2. **`pgbench-c`** (`workloads/pgbench-c/`): 9-table wholesale supplier order-entry OLTP workload (inspired by TPC-C).
3. **`pgbench-e`** (`workloads/pgbench-e/`): 16-table financial brokerage OLTP workload (inspired by TPC-E).
4. **`pgbench-h`** (`workloads/pgbench-h/`): 8-table decision support analytical workload with concurrent refresh stream (inspired by TPC-H).
5. **`pgbench-ds`** (`workloads/pgbench-ds/`): 13-table multi-channel star-schema retail decision support workload with concurrent fact maintenance (inspired by TPC-DS).
6. **`pgbench-htap`** (`workloads/pgbench-htap/`): Hybrid operational + analytical workload running concurrent OLTP and OLAP client groups (inspired by the CH-benCHmark specification).

> **Disclaimer**: All `pgbench-*` suites are **non-comparable, inspired-by engineering workloads**. Results represent un-audited internal engineering measurements and do not constitute official certified TPC or CH-benCHmark results. Never report trademarked council metrics (`tpmC`, `QphH`, `QphDS`); always report generic engineering metrics (`TPS`, `QPM`, `orders/sec`, `trades/sec`, and HDR latency percentiles `p50`/`p95`/`p99`).

---

## 1. The 3-Stage Parallel In-Database Initialization Pipeline

All six relational suites generate 100% of their synthetic data **inside PostgreSQL** using `generate_series()` and deterministic C functions from `contrib/pgbench` (`pgbench.random`, `pgbench.nurand`, `pgbench.c_last`, `pgbench.random_astring`, `pgbench.date_add`, etc.). No external data generator binaries or flat CSV staging files are needed.

### 1.1 Single-Command Initialization via `--config=<file> -i` (Recommended)

Every suite directory contains a declarative `.pgbench.json` file that defines the three initialization stages (`schema` $\rightarrow$ parallel `data` $\rightarrow$ `constraints`). Run all three stages in a single command from any directory:

```bash
createdb bench_c
pgbench --config=workloads/pgbench-c/pgbench_c.pgbench.json -i -s 4 bench_c
```

### 1.2 Manual 3-Stage Initialization (`-n -t 1`)

You can also execute the three SQL stages explicitly (for example, to customize the number of parallel loader workers `W`):

```bash
export W=4
export SCALE=4

# Stage 1: DDL Schema & Stored Procedures (-n -t 1 is mandatory!)
# Automatically executes: CREATE EXTENSION IF NOT EXISTS pgbench;
pgbench -n -t 1 -f workloads/pgbench-c/pgbench_c_schema.sql bench_c

# Stage 2: Parallel Data Population across W workers (-c $W -j $W -t 1)
# Partitioned lock-free across workers using :client_id and -D num_workers=$W
pgbench -n -c $W -j $W -t 1 -s $SCALE -D scale=$SCALE -D num_workers=$W \
    -f workloads/pgbench-c/pgbench_c_init.sql bench_c

# Stage 3: Primary Keys, Secondary Indexes, Foreign Keys & ANALYZE (-n -t 1)
pgbench -n -t 1 -f workloads/pgbench-c/pgbench_c_constraints.sql bench_c
```

> **Why `-n -t 1` is required in manual initialization**:
> - Without `-n` (`--no-vacuum`), `pgbench` tries to `VACUUM pgbench_branches` before running the script, which fails on non-default schemas.
> - Without `-t 1`, `pgbench` defaults to running `10` transactions per client (`-t 10`), executing the DDL or constraint script 10 times.

---

## 2. `pgbench-b` — Stored-Procedure Banking OLTP (`workloads/pgbench-b/`)

### Overview
Modeled after classic banking OLTP transactions (and `pgbench`'s built-in `tpcb-like` workload), `pgbench-b` encapsulates the multi-statement debit/credit transaction inside server-side SQL/PLpgSQL stored procedures (`CALL pgbench_b_transaction(...)` and `CALL pgbench_b_simple_update(...)`). This eliminates 7 client-server network round-trips per transaction and stresses PostgreSQL's stored procedure execution and row-level locking engine.

### Schema & Scripts
- **Tables (4)**: `pgbench_branches`, `pgbench_tellers`, `pgbench_accounts` (`100,000` rows $\times$ `scale`), `pgbench_history`.
- **Procedures (2)**: `pgbench_b_transaction(aid, tid, bid, delta)`, `pgbench_b_simple_update(aid, tid, bid, delta)`.
- **Workload Scripts**:
  - `pgbench_b_tx.sql`: Full banking transaction via `CALL pgbench_b_transaction(...)`.
  - `pgbench_b_simple_update.sql`: Simple account update via `CALL pgbench_b_simple_update(...)`.
  - `pgbench_b_select_only.sql`: Point lookup on `pgbench_accounts`.

### Initialize & Run
```bash
createdb bench_b

# Initialize at scale 10 (1,000,000 accounts)
pgbench --config=workloads/pgbench-b/pgbench_b.pgbench.json -i -s 10 bench_b

# Run for 60 seconds with 16 clients, 4 threads, and SLA verification
pgbench --config=workloads/pgbench-b/pgbench_b.pgbench.json \
    -s 10 -c 16 -j 4 -T 60 \
    --sla="total:p95<=50ms" --sla="total:error_rate<=0%" \
    --json-report=report_b.json bench_b
```

---

## 3. `pgbench-c` — Wholesale Order-Entry OLTP (`workloads/pgbench-c/`)

### Overview
Modeled after the TPC-C wholesale supplier order-entry specification, `pgbench-c` exercises skewed non-uniform random access (`pgbench.nurand`), syllable-based customer last-name lookups (`pgbench.c_last`), multi-warehouse distributed stock updates, intentional 1% invalid-item rollbacks handled gracefully via `\try ... \catch`, and realistic terminal `\keying_time` and `\think_time` pacing.

### Schema & Transaction Mix
- **Tables (9)**: `warehouse` (`scale` rows), `district` (`10` per warehouse), `customer` (`3,000` per district), `history`, `new_order`, `orders`, `order_line`, `item` (`100,000` rows), `stock` (`100,000` per warehouse).
- **Stored Procedures (5) & Weighted Scripts**:

| Script File | Weight | Stored Procedure Called | Key Workload Behavior |
| :--- | :---: | :--- | :--- |
| `pgbench_c_new_order.sql` | **45%** | `CALL pgbench_c_new_order(...)` | Inserts order with 5–15 line items; 1% of transactions generate an invalid item ID and roll back via `\try ... \catch` (recorded in `expected_rollbacks`, not `transactions_failed`). |
| `pgbench_c_payment.sql` | **43%** | `CALL pgbench_c_payment(...)` | Updates warehouse, district, and customer balances; 60% of lookups query customer by `c_last(nurand(255, 0, 999))`; 15% access a remote warehouse when `scale > 1`. |
| `pgbench_c_order_status.sql` | **4%** | `CALL pgbench_c_order_status(...)` | Read-only lookup of a customer's most recent order and its order lines. |
| `pgbench_c_delivery.sql` | **4%** | `CALL pgbench_c_delivery(...)` | Batch processing across districts `1..10` using a client-side `\for d_id 1 .. 10` loop. |
| `pgbench_c_stock_level.sql` | **4%** | `CALL pgbench_c_stock_level(...)` | Read-only scan counting recent order items whose stock fell below a random threshold. |

### Initialize & Run
> **Scale Factor Tip**: Always use `scale >= 2` (`-s 2` or higher) when testing `pgbench-c` so that multi-warehouse remote stock and payment code paths (`\if :scale > 1`) are exercised.

```bash
createdb bench_c

# Initialize 4 warehouses in parallel
pgbench --config=workloads/pgbench-c/pgbench_c.pgbench.json -i -s 4 bench_c

# Run the 5-script weighted mix via --config
pgbench --config=workloads/pgbench-c/pgbench_c.pgbench.json \
    -s 4 -c 16 -j 4 -T 60 \
    --json-report=report_c.json bench_c
```

---

## 4. `pgbench-e` — Financial Brokerage OLTP (`workloads/pgbench-e/`)

### Overview
Modeled after the TPC-E financial brokerage specification, `pgbench-e` simulates customers, brokers, securities exchanges, market feeds, and trade execution across a rich relational schema with foreign keys, check constraints, and 10 stored procedures.

### Schema & Transaction Mix
- **Tables (16)**: `account_permission`, `customer`, `customer_account`, `customer_taxrate`, `holding`, `holding_history`, `holding_summary`, `watch_item`, `watch_list`, `broker`, `cash_transaction`, `charge`, `commission_rate`, `settlement`, `trade`, `trade_history`, `trade_request`, `trade_type`, `company`, `daily_market`, `exchange`, `financial`, `industry`, `last_trade`, `news_item`, `news_xref`, `sector`, `security`, `status_type`, `taxrate`, `zip_code` (consolidated into 16 core operational tables).
- **Weighted Scripts (6)**:
  - `pgbench_e_trade_order.sql` (**10%**): Submits market or limit trade orders (`CALL pgbench_e_trade_order(...)`), with 1% intentional rollback caught via `\try`/`\catch`.
  - `pgbench_e_trade_result.sql` (**10%**): Completes trade settlement, updates holdings, and triggers Market-Feed updates (`CALL pgbench_e_trade_result(...)` + `CALL pgbench_e_market_feed(...)`).
  - `pgbench_e_customer_position.sql` (**13%**): Reads customer account valuations and security details.
  - `pgbench_e_market_watch.sql` (**18%**): Computes percentage change across a customer's watch list, industry, or company index.
  - `pgbench_e_broker_volume.sql` (**5%**): Aggregates pending trade volume across a sector for a group of brokers.
  - `pgbench_e_trade_status.sql` (**44%**): High-frequency read lookup of recent trade statuses and trade history.

### Initialize & Run
```bash
createdb bench_e

# Initialize at scale 2
pgbench --config=workloads/pgbench-e/pgbench_e.pgbench.json -i -s 2 bench_e

# Execute the 6-script brokerage mix
pgbench --config=workloads/pgbench-e/pgbench_e.pgbench.json \
    -s 2 -c 8 -j 4 -T 60 \
    --json-report=report_e.json bench_e
```

---

## 5. `pgbench-h` — Decision Support OLAP + Refresh (`workloads/pgbench-h/`)

### Overview
Modeled after the TPC-H decision support specification, `pgbench-h` combines complex multi-join analytical queries (`Q1`, `Q3`, `Q5`, `Q6`, `Q10`, `Q12`, `Q14`, `Q19`) with a concurrent, rate-throttled refresh stream (`RF1` bulk lineitem/orders insert + `RF2` old order delete) using two heterogeneous `--group` pools.

### Schema & Workload Groups
- **Tables (8)**: `region`, `nation`, `supplier`, `customer`, `part`, `partsupp`, `orders`, `lineitem`.
- **Client Groups (2)**:
  1. `queries` (`sequential=true`): Executes `pgbench_h_queries.sql` (or individual `pgbench_h_q*.sql` scripts) in permuted stream order.
  2. `refresh` (`rate=2`): Executes `pgbench_h_refresh.sql` (`RF1` + `RF2` write refresh pair) throttled to 2 refreshes/sec.

### Initialize & Run
```bash
createdb bench_h

# Initialize at scale 1
pgbench --config=workloads/pgbench-h/pgbench_h.pgbench.json -i -s 1 bench_h

# Run concurrent OLAP query streams + throttled RF1/RF2 refresh stream
pgbench --config=workloads/pgbench-h/pgbench_h.pgbench.json \
    -T 60 --json-report=report_h.json bench_h

# Or run with explicit --group flags:
pgbench -n -T 60 -s 1 -D scale=1 \
    --group="name=queries,clients=4,threads=4,sequential=true,file=workloads/pgbench-h/pgbench_h_queries.sql" \
    --group="name=refresh,clients=1,threads=1,rate=2,file=workloads/pgbench-h/pgbench_h_refresh.sql" \
    --sla="queries:p95<=30000ms" \
    --json-report=report_h.json bench_h
```

---

## 6. `pgbench-ds` — Multi-Channel Star-Schema OLAP (`workloads/pgbench-ds/`)

### Overview
Modeled after the TPC-DS multi-channel retail decision support specification, `pgbench-ds` tests star-schema joins, common table expressions (CTEs), window rankings (`RANK() OVER (...)`), rollup aggregations across store, catalog, and web sales channels, and concurrent fact table maintenance.

### Schema & Workload Groups
- **Tables (13)**:
  - **Dimensions (8)**: `date_dim`, `item`, `customer_address`, `customer_demographics`, `customer`, `store`, `warehouse`, `promotion`.
  - **Facts (5)**: `store_sales`, `store_returns`, `catalog_sales`, `web_sales`, `inventory`.
- **Client Groups (2)**:
  1. `reporting`: Runs an equally weighted mix (25% each) of:
     - `pgbench_ds_q_store_sales.sql` (Store sales & returns rollup with CTEs)
     - `pgbench_ds_q_cross_channel.sql` (Cross-channel store/catalog/web window ranking)
     - `pgbench_ds_q_inventory.sql` (Warehouse inventory variation analysis)
     - `pgbench_ds_q_promotion.sql` (Promotional sales ratio analysis)
  2. `maint` (`rate=2`): Runs `pgbench_ds_maintenance.sql` (concurrent fact inserts and returns cleanup).

### Initialize & Run
```bash
createdb bench_ds

# Initialize star schema at scale 1
pgbench --config=workloads/pgbench-ds/pgbench_ds.pgbench.json -i -s 1 bench_ds

# Run reporting + maintenance groups
pgbench --config=workloads/pgbench-ds/pgbench_ds.pgbench.json \
    -T 60 --json-report=report_ds.json bench_ds
```

---

## 7. `pgbench-htap` — Hybrid OLTP + OLAP (`workloads/pgbench-htap/`)

### Overview
Modeled after the **CH-benCHmark** hybrid transactional/analytical processing specification, `pgbench-htap` unifies the 9 operational order-entry tables of `pgbench-c` with 3 analytical supplier/geography dimension tables (`htap_region`, `htap_nation`, `htap_supplier`). It runs high-concurrency operational transactions (`oltp` group) simultaneously against heavy analytical scans (`olap` group) on the exact same live tables (`orders`, `order_line`, `stock`, `customer`), measuring MVCC snapshot isolation overhead, buffer pool contention, and per-group SLA compliance.

### Schema & Dual Client Groups
- **Tables (12)**: All 9 `pgbench-c` tables (`warehouse`, `district`, `customer`, `history`, `new_order`, `orders`, `order_line`, `item`, `stock`) + 3 HTAP analytical tables (`htap_region`, `htap_nation`, `htap_supplier`).
- **Group 1 — `oltp`**: Executes `pgbench_htap_oltp.sql` (full weighted mix of `New-Order`, `Payment`, `Order-Status`, `Delivery`, and `Stock-Level` stored procedures).
- **Group 2 — `olap`**: Executes an equally weighted mix (20% each) of 5 analytical queries over the live operational tables:
  - `pgbench_htap_olap_q1.sql`: `order_line` pricing and quantity summary report.
  - `pgbench_htap_olap_q3.sql`: Unshipped order revenue priority across `customer`, `orders`, and `order_line`.
  - `pgbench_htap_olap_q5.sql`: Regional supplier revenue joining `customer`, `orders`, `order_line`, `stock`, `htap_supplier`, `htap_nation`, and `htap_region`.
  - `pgbench_htap_olap_q6.sql`: Forecasting revenue change scan over `order_line`.
  - `pgbench_htap_olap_q18.sql`: Large-volume customer aggregation (`GROUP BY ... HAVING sum(ol_quantity) > ...`).

### Initialize & Run with Independent Per-Group SLAs
```bash
createdb bench_htap

# Initialize combined operational + analytical schema at scale 4
pgbench --config=workloads/pgbench-htap/pgbench_htap.pgbench.json -i -s 4 bench_htap

# Run simultaneous OLTP (8 clients) and OLAP (2 clients) groups with independent SLAs
pgbench --config=workloads/pgbench-htap/pgbench_htap.pgbench.json \
    -T 60 \
    --sla="oltp:p95<=5000ms" \
    --sla="olap:p95<=10000ms" \
    --sla="total:error_rate<=0%" \
    --log-db=self --run-label="htap-scale4" \
    --json-report=report_htap.json bench_htap
```

---

## 8. Fast Verification & Density Overrides

For rapid CI smoke testing (`< 10s` per suite), you can initialize at `scale=1` (or `scale=2` for `pgbench-c` / `pgbench-htap`) or pass custom row-density variables via `-D` during Stage 2 (`<suite>_init.sql`) and runtime execution:

| Suite | Supported `-D` Density Variables (with defaults) |
| :--- | :--- |
| **`pgbench-b`** | `-D scale=1` (`100,000` accounts per scale) |
| **`pgbench-c` / `pgbench-htap`** | `-D scale=2 -D items_per_scale=100000 -D dist_per_wh=10 -D cust_per_dist=3000 -D orders_per_dist=3000` |
| **`pgbench-e`** | `-D scale=1 -D cust_per_scale=500 -D accts_per_cust=2 -D trades_per_acct=10` |
| **`pgbench-h`** | `-D scale=1 -D supp_per_scale=100 -D part_per_scale=200 -D cust_per_scale=150 -D orders_per_scale=1500` |
| **`pgbench-ds`** | `-D scale=1 -D items_per_scale=200 -D cust_per_scale=500 -D sales_per_scale=2000` |
