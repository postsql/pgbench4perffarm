<!--
  DISCLAIMER & INSPIRATION NOTE:
  The pgbench-order (pgbench-c) wholesale order-entry workload is an independent,
  clean-room engineering benchmark inspired by TPC-C concepts. It is NOT an
  official industry standard benchmark, has NOT been audited or certified by any
  standards council, and does NOT produce certified ratings.
  Per our trademark guidelines (src/bin/pgbench/workloads/README and
  src/bin/pgbench/workloads/BENCHMARK_INSPIRATION.md), do not use council
  trademarked names or metrics when referring to this benchmark; everywhere in
  prose, headings, and scripts, use pgbench-order (or pgbench-c) and standard
  engineering metrics (TPS, QPM, orders/sec, ops/sec, and HDR latency
  percentiles).
-->
# Wholesale Order-Entry Workload (`pgbench-order` / `pgbench-c`) Specification & Multi-Dialect / Non-SQL Porting Reference

This reference provides the complete, self-contained specification of the wholesale order-entry workload (`src/bin/pgbench/workloads/pgbench-order/`, legacy alias `workloads/pgbench-c/`) so an AI coding agent can re-create the schema, initial data generator, skewed parameter distributions, all 5 transactions (`New-Order`, `Payment`, `Order-Status`, `Delivery`, `Stock-Level`), and post-run consistency audits in **any SQL dialect** or **any non-SQL query language**.

> **Guardrail**: Per `src/bin/pgbench/workloads/README`, never claim official council compliance or publish trademarked council performance metrics in benchmark reports; report standard engineering metrics (`TPS`, `QPM`, `orders/sec`, `ops/sec`, and HDR latency percentiles `p50`, `p95`, `p99`, `p99.9`).

---

## 1. Why `pgbench` Client Scripts Are Dialect- & Language-Agnostic

All random distributions and string/array generators in `src/common/pgbench_funcs.c` are built directly into the `pgbench` client expression engine (`exprparse.y`):
- **Distributions**: `random(min, max)`, `nurand(A, min, max [, C])`, `random_exponential(min, max, param)`
- **Domain Helpers**: `c_last(0..999)`, `permute(val, size, seed)`, `random_astring(min, max)`, `random_nstring(min, max)`, `format_string(fmt, ...)`, `date_add(base, offset, unit)`, `array_generate(len, val)`
- **Control Flow & Result Binding**: `\set`, `\if`/`\elif`/`\else`/`\endif`, `\for <v> <s> .. <e> BY 1`/`\endfor`, `\while`/`\endwhile`, `\gset`, `\gset_col`, `\try`/`\catch`/`\endtry`, `\keying_time`, `\think_time`, `\validate_result`

Consequently, even when the target database has **no `contrib/pgbench` extension**, **no stored procedures**, or **is not a SQL database** (e.g., Neo4j Cypher via `--driver=neo4j`, Document MQL, or Key-Value APIs), the `pgbench` client can compute 100% of transaction parameters, loop iterations, and rollback control flow on the client side.

---

## 2. The 9 Entities, Scaling Rules & Keys

Let $W = \text{scale}$ (warehouses, use $W \ge 2$ to exercise remote warehouse transactions), $D_{\text{cust}} = \text{cust\_per\_dist}$ (`30` default fast mode; `3000` full spec), and $I_{\text{scale}} = \text{items\_per\_scale}$ (`1000` default fast mode; `100000` full spec).

| # | Entity | Cardinality | Primary Key | Secondary Index | Key Columns & Initial Values |
| :---: | :--- | :--- | :--- | :--- | :--- |
| **1** | `warehouse` | $W$ | `(w_id)` | — | `w_id` ($1..W$), `w_name` (`'WH-%04d'`), `w_street_1/2`, `w_city`, `w_state` (`char(2)`), `w_zip` (`char(9)`), `w_tax` ($[0, 0.2000]$), `w_ytd = 300000.00` |
| **2** | `district` | $10 W$ | `(d_w_id, d_id)` | — | `d_id` ($1..10$), `d_w_id` ($1..W$), `d_name` (`'DIST-%02d-%02d'`), `d_tax` ($[0, 0.2000]$), `d_ytd = 30000.00`, `d_next_o_id = D_cust + 1` |
| **3** | `customer` | $10 W D_{\text{cust}}$ | `(c_w_id, c_d_id, c_id)` | `(c_w_id, c_d_id, c_last, c_first)` | `c_id` ($1..D_{\text{cust}}$), `c_middle='OE'`, `c_last` (`c_last((c_id-1)%1000)` for $\le 1000$, else `c_last(nurand(255,0,999))`), `c_credit` (`'BC'` 10%, `'GC'` 90%), `c_credit_lim=50000.00`, `c_discount` ($[0, 0.5000]$), `c_balance=-10.00`, `c_ytd_payment=10.00`, `c_payment_cnt=1`, `c_delivery_cnt=0`, `c_data` (`varchar(500)`) |
| **4** | `history` | $10 W D_{\text{cust}}$ | *(none or surrogate)* | — | `h_c_id, h_c_d_id, h_c_w_id, h_d_id, h_w_id, h_date, h_amount=10.00, h_data` (`varchar(24)`) |
| **5** | `new_order` | $10 W \lfloor 0.3 D_{\text{cust}} \rfloor$ | `(no_w_id, no_d_id, no_o_id)` | — | Contains last 30% of initial orders per district ($o\_id \in [\lfloor 0.7 D_{\text{cust}} \rfloor + 1, D_{\text{cust}}]$) |
| **6** | `orders` | $10 W D_{\text{cust}}$ | `(o_w_id, o_d_id, o_id)` | `(o_w_id, o_d_id, o_c_id, o_id DESC)` | `o_c_id = permute(o_id-1, D_cust, w_id*100+d_id)+1`, `o_carrier_id = random(1,10)` if $o\_id \le \lfloor 0.7 D_{\text{cust}} \rfloor$ else `NULL`, `o_ol_cnt = 5 + ((o_id+d_id+w_id)%6)`, `o_all_local = 1` |
| **7** | `order_line` | $\approx 7.5 \times 10 W D_{\text{cust}}$ | `(ol_w_id, ol_d_id, ol_o_id, ol_number)` | — | `ol_number` ($1..o\_ol\_cnt$), `ol_i_id = random(1, I_scale)`, `ol_supply_w_id = w_id`, `ol_quantity = 5`, `ol_delivery_d` (set if delivered, else `NULL`), `ol_amount` (`0.00` if delivered, else $[0.10, 50.00]$), `ol_dist_info` (`char(24)`) |
| **8** | `item` | $I_{\text{scale}}$ | `(i_id)` | — | `i_id` ($1..I_{\text{scale}}$), `i_im_id` ($1..10000$), `i_name`, `i_price` ($[1.00, 100.00]$), `i_data` (`varchar(50)`, 10% containing `'ORIGINAL'`) |
| **9** | `stock` | $W \times I_{\text{scale}}$ | `(s_w_id, s_i_id)` | — | `s_quantity` ($[10, 100]$), `s_dist_01`..`s_dist_10` (`char(24)`), `s_ytd=0`, `s_order_cnt=0`, `s_remote_cnt=0`, `s_data` (10% `'ORIGINAL'`) |

---

## 3. Exact Mathematical Primitives

1. **`nurand(A, x, y [, C])`**:
   $$\text{NURand}(A, x, y) = \bigl(((\text{random}(0, A) \mathbin{|} \text{random}(x, y)) + C) \bmod (y - x + 1)\bigr) + x$$
   - `c_last`: `nurand(255, 0, 999)`
   - `c_id`: `nurand(1023, 1, :cust_per_dist)` (`30` default; `3000` full)
   - `i_id`: `nurand(8191, 1, :items_per_scale)` (`1000` default; `100000` full)
2. **`c_last(num)`** ($0 \le num \le 999$):
   - Syllables $S_0 \dots S_9$: `["BAR", "OUGHT", "ABLE", "PRI", "PRES", "ESE", "ANTI", "CALLY", "ATION", "EING"]`
   - Formula: $S_{\lfloor num / 100 \rfloor \bmod 10} \,\|\, S_{\lfloor num / 10 \rfloor \bmod 10} \,\|\, S_{num \bmod 10}$.

---

## 4. Exact Specification of the 5 Transactions (`45 : 43 : 4 : 4 : 4` Mix)

All 5 scripts begin with `\keying_time 1 ms` and end with `\think_time random_exponential(1, 5, 2.0) ms`.

### 4.1 `New-Order` (Weight: `45%`)
- **Inputs**: `w_id = random(1, :scale)`, `d_id = random(1, 10)`, `c_id = nurand(1023, 1, 30)`, `ol_cnt = random(5, 15)`, `rbk = random(1, 100)`.
  - For `idx` in `0 .. (ol_cnt - 1)`: `i_ids[idx] = nurand(8191, 1, 1000)`, `s_w_ids[idx] = w_id` (if `:scale > 1` and `random(1, 100) = 1`, pick remote warehouse $\ne w\_id$), `qtys[idx] = random(1, 10)`.
  - If `rbk = 1` (1% of New-Orders): set `i_ids[ol_cnt - 1] = -1`.
- **State Transitions (Atomic)**:
  1. Read `w_tax` from `warehouse(w_id)`.
  2. Increment `district.d_next_o_id = d_next_o_id + 1` for `(w_id, d_id)`, capturing old `d_next_o_id` as `v_o_id` and `d_tax`.
  3. Read `c_discount, c_last, c_credit` from `customer(w_id, d_id, c_id)`.
  4. Insert `orders(v_o_id, d_id, w_id, c_id, NOW, NULL, ol_cnt, all_local)` and `new_order(v_o_id, d_id, w_id)`.
  5. For each line `idx = 1 .. ol_cnt`:
     - Read `i_price, i_name, i_data` from `item(i_id)`. **If not found (`i_id = -1`), abort/rollback entire transaction** inside `\try ... \catch '22012'` (increments `expected_rollbacks`).
     - Update `stock(s_w_id, i_id)`: `s_quantity = (s_quantity - qty >= 10) ? (s_quantity - qty) : (s_quantity - qty + 91)`, `s_ytd += qty`, `s_order_cnt += 1`, `s_remote_cnt += (s_w_id != w_id ? 1 : 0)`, reading `s_dist_<d_id>`.
     - Insert `order_line` with `ol_amount = round(qty * i_price * (1 + w_tax + d_tax) * (1 - c_discount), 2)`.

### 4.2 `Payment` (Weight: `43%`)
- **Inputs**: `w_id = random(1, :scale)`, `d_id = random(1, 10)`, `h_amount = random(100, 500000) / 100.0`.
  - 85% local `(c_w_id, c_d_id) = (w_id, d_id)`; 15% remote `c_w_id != w_id, c_d_id = random(1, 10)` when `:scale > 1`.
  - 60% by `c_last = c_last(nurand(255, 0, 999))`; 40% by `c_id = nurand(1023, 1, 30)`.
- **State Transitions (Atomic)**:
  1. `warehouse.w_ytd += h_amount` (read `w_name`); `district.d_ytd += h_amount` (read `d_name`).
  2. If by `c_last`: count matching customers in `(c_w_id, c_d_id, c_last)`; if `v_cnt > 0`, pick row at `OFFSET (v_cnt - 1) / 2` ordered by `c_first ASC`; if `v_cnt == 0` (small scale), fallback to `ORDER BY (c_last >= p_c_last) DESC, c_last, c_first LIMIT 1`.
  3. Update `customer`: `c_balance -= h_amount`, `c_ytd_payment += h_amount`, `c_payment_cnt += 1`. If `c_credit = 'BC'` (checked via unquoted `\if :c_credit = 'BC'`), prepend `format_string('%d %d %d %d %d %.2f | %s', v_c_id, c_d_id, c_w_id, d_id, w_id, h_amount, c_data)` truncated to 500 chars into `c_data`.
  4. Insert `history` with `h_data = left(w_name || '    ' || d_name, 24)`.

### 4.3 `Order-Status` (Weight: `4%`)
- **Inputs**: `w_id = random(1, :scale)`, `d_id = random(1, 10)`, 60% `c_last` / 40% `c_id`.
- **State Transitions (Read-Only)**:
  1. Resolve customer `v_c_id` (same 60% `c_last` midpoint / 40% `c_id` rule) and read `c_balance, c_first, c_middle, c_last`.
  2. Find latest order `o_id, o_entry_d, o_carrier_id` for `(w_id, d_id, v_c_id)` (`ORDER BY o_id DESC LIMIT 1`).
  3. Read/count `order_line` rows for `(w_id, d_id, v_o_id)`.

### 4.4 `Delivery` (Weight: `4%`)
- **Inputs**: `w_id = random(1, :scale)`, `o_carrier_id = random(1, 10)`, `\for d_id 1 .. 10 BY 1`.
- **State Transitions (Per District `1..10`)**:
  1. Lock the district row (`SELECT d_id FROM district WHERE d_w_id = :w_id AND d_id = :d_id FOR UPDATE \gset`) or use atomic `DELETE ... RETURNING` so concurrent `Delivery` transactions never pop the same order, then delete the oldest `new_order` row `no_o_id = MIN(no_o_id)` in `(w_id, d_id)`. If empty, skip district cleanly.
  2. Update `orders` set `o_carrier_id = :o_carrier_id`, reading `o_c_id`.
  3. Update `order_line` set `ol_delivery_d = CURRENT_TIMESTAMP` and sum `ol_amount` as `v_ol_total`.
  4. Update `customer` set `c_balance += v_ol_total, c_delivery_cnt += 1`.

### 4.5 `Stock-Level` (Weight: `4%`)
- **Inputs**: `w_id = random(1, :scale)`, `d_id = random(1, 10)`, `threshold = random(10, 20)`.
- **State Transitions (Read-Only)**:
  1. Read `v_next_o_id = d_next_o_id` from `district(w_id, d_id)`.
  2. Count `DISTINCT s_i_id` from `order_line JOIN stock ON s_w_id = ol_w_id AND s_i_id = ol_i_id` where `ol_w_id = :w_id AND ol_d_id = :d_id AND ol_o_id >= v_next_o_id - 20 AND ol_o_id < v_next_o_id AND s_quantity < :threshold`.

---

## 5. Server-Side Stored Procedures & `contrib/pgbench` C Functions (`Model A`) — Why Server-Side Execution Is Preferable for Performant Systems

When targeting PostgreSQL (or Oracle `PL/SQL`, SQL Server `T-SQL`, DB2 `SQL PL`) and **a performant system is desired**, **always prefer `Model A` (Server-Side Stored Procedures + In-Database C Functions)** over **`Model B` (Client-Driven Multi-Statement Interactive SQL)** (companion full guide in `doc/agent-guide-pgbench-order.md`):

### 5.1 Four Architectural Reasons `Model A` Outperforms `Model B`
1. **Network Round-Trip Collapse (`1.36` vs. `29.57` RTTs/tx — `21.7x` Reduction)**:
   - `Model A` executes `New-Order` (`1` `CALL`), `Payment` (`1` `CALL`), `Order-Status` (`1` `CALL`), `Delivery` (`10` `CALL`s), and `Stock-Level` (`1` `CALL`), averaging **`1.36` network RTTs per transaction** across the `45:43:4:4:4` mix.
   - `Model B` cannot pipeline statements because intermediate variables (`v_o_id`, `w_tax`, `d_tax`, `c_discount`, `i_price`, `s_quantity`, `s_dist_XX`, `v_cnt`, `v_c_id`, `v_ol_total`) must be fetched via `\gset` before the next query can be bound, requiring **`47` RTTs per `New-Order`** (or `26` with `RETURNING`), **`10.6` RTTs per `Payment`**, **`90` RTTs per `Delivery`**, and **`29.57` weighted RTTs per transaction** (adding $+14.1\text{ ms/tx}$ at $\text{RTT}=0.5\text{ ms}$ intra-AZ or $+56.4\text{ ms/tx}$ at $\text{RTT}=2.0\text{ ms}$ cross-AZ).
2. **Dramatically Shorter Heavyweight Row-Lock Hold Times on Hot Rows (`warehouse`, `district`, `stock`)**:
   - `warehouse(w_id)` is updated by **100% of `Payment` transactions** (`43%` of mix), and `district(d_w_id, d_id)` is updated by **100% of `New-Order` (`45%`) and `Payment` (`43%`) transactions**.
   - In `Model B`, after acquiring the exclusive row lock on `district(d_w_id, d_id)` or `warehouse(w_id)`, the backend sits idle waiting for **`8` to `44` client-server network round-trips** before `COMMIT`. Network latency is injected directly into the critical section ($T_{\max} \le 1 / W_{\text{lock}}$), causing severe `ProcSleep` lock-queue convoys.
   - In `Model A`, the entire transaction runs inside the backend process via SPI in microseconds/low milliseconds with **zero network I/O while holding tuple locks**, compressing lock hold time by **10x–50x**.
   - **Measured `ProcSleep` / `ShareLock` Telemetry (`log_lock_waits = on`, `deadlock_timeout = 10ms`)**: Across `1,000` mixed transactions at `-s 4 -c 4 -j 2` on PostgreSQL 20devel, `Model B` triggered **`16+` heavyweight tuple lock waits exceeding `10 ms` (up to `24.8 ms`) on `district` and `warehouse` rows**, whereas `Model A` recorded **`0` heavyweight lock waits `> 10 ms` on `district` or `warehouse` rows**.
   - **Handling & Eliminating `40P01` (`deadlock_detected`) on Multi-Line `stock` Updates**: Because `New-Order` draws `5..15` skewed `i_ids` via `nurand(8191, 1, 1000)` in unsorted order, concurrent `New-Order` transactions on the same warehouse can lock overlapping `stock(s_w_id, s_i_id)` tuples in opposite order (`SQLSTATE 40P01`). Pass `--max-tries=10` (or `"max_tries": 10` in `.pgbench.json`) so `pgbench` transparently retries any `40P01` deadlock (`transactions_failed == 0`), or pre-lock `(s_w_id, s_i_id)` in deterministic `(s_w_id ASC, s_i_id ASC)` order inside `pgbench_order_new_order`.
3. **Backend SPI Plan Caching (`SPIPlanPtr`) & Single-Pass `UPDATE ... RETURNING`**:
   - `PL/pgSQL` automatically prepares and caches `SPIPlanPtr` plans in backend private memory for every embedded SQL statement—even when the client or connection pooler (`PgBouncer`) sends `CALL pgbench_order_*(...)` using the **Simple Query Protocol (`-M simple`)**.
   - Inside `PL/pgSQL`, `UPDATE district SET d_next_o_id = d_next_o_id + 1 ... RETURNING d_next_o_id - 1, d_tax INTO v_o_id, v_d_tax`, `UPDATE stock ... RETURNING s_quantity, s_data, CASE p_d_id ... INTO ...`, and `DELETE FROM new_order ... RETURNING no_o_id INTO v_o_id` combine read-lock-modify into a single index probe and heap pin instead of split `SELECT ... FOR UPDATE \gset` + `UPDATE` round-trips.
4. **Set-Based In-Database Initialization via Compiled C Functions (`6.8x` Faster Load)**:
   - `pgbench_order_init.sql` (`pgbench_order_init.sql`) combines `generate_series()` / `CROSS JOIN LATERAL` with compiled C functions in `contrib/pgbench` (`pgbench.setseed()`, `pgbench.random()`, `pgbench.nurand()`, `pgbench.c_last()`, `pgbench.permute()`, `pgbench.random_astring()`, `pgbench.random_nstring()`, `pgbench.format_string()`, `pgbench.date_add()`) to populate all 9 tables in **`9` bulk `INSERT INTO ... SELECT` statements per worker** (`338.9 ms` at `-s 4` vs. `2,317.3 ms` for `~16,640` client-driven `INSERT`s).

### 5.2 Measured Performance Comparison on PostgreSQL 20devel (`-s 4`, All 8 Invariants Verified)

| Benchmark Scenario | `Model A` (`PL/pgSQL` + `contrib/pgbench`) | `Model B` (Pure Client-Driven SQL) | `Model A` Advantage |
| :--- | :--- | :--- | :--- |
| **Parallel Data Init (`-s 4`, `-c 4 -j 4`)** | **`338.9 ms`** (`36` bulk statements) | `2,317.3 ms` (`~16,640` `INSERT`s, `-M prepared`) | **`6.8x` faster initialization** |
| **1-Client Unpaced (`-c 1`, Unix Socket, `-M simple`)** | **`190.0 TPS`** (`p50 = 2.13 ms`, `p95 = 13.25 ms`) | `39.7 TPS` (`p50 = 22.78 ms`, `p95 = 60.93 ms`) | **`4.8x` higher TPS**, **`10.7x` lower `p50`** |
| **1-Client Unpaced (`-c 1`, Unix Socket, `-M prepared`)** | **`182.6 TPS`** (`p50 = 3.22 ms`, `p99 = 16.00 ms`) | `93.3 TPS` (`p50 = 5.79 ms`, `p99 = 34.30 ms`) | **`2.0x` higher TPS**, **`2.1x` lower `p99`** |
| **8-Client Unpaced (`-c 8 -j 4`, TCP `127.0.0.1`, `-M simple`)** | **`842.4 TPS`** (`p50 = 7.58 ms`, `p99 = 36.10 ms`) | `241.7 TPS` (`p50 = 26.24 ms`, `p99 = 135.17 ms`) | **`3.5x` higher TPS**, **`3.7x` lower `p99`** |
| **8-Client Unpaced (`-c 8 -j 4`, TCP `127.0.0.1`, `-M prepared`)** | **`824.1 TPS`** (`p50 = 7.68 ms`, `p99 = 38.06 ms`) | `520.1 TPS` (`p50 = 12.26 ms`, `p99 = 67.07 ms`) | **`1.6x` higher TPS**, **`1.8x` lower `p99`** |
| **8-Client Paced (`-c 8 -j 4`, `\keying_time` + `\think_time`)** | **`499.7 TPS`** (`p50 = 7.36 ms`, `p99 = 35.33 ms`) | `386.8 TPS` (`p50 = 11.44 ms`, `p99 = 69.46 ms`) | **`1.3x` higher TPS**, **`2.0x` lower `p99`** |

#### 5.2.1 Decision Matrix: When to Choose `Model A` vs. `Model B` vs. `Model C`

| Decision Criterion | `Model A` (Server-Side Stored Procedures + `contrib/pgbench`) | `Model B` (Pure Client-Driven Multi-Statement SQL) | `Model C` (Non-SQL Query Language) |
| :--- | :--- | :--- | :--- |
| **Primary Goal** | **Maximum SUT throughput, lowest `p99` tail latency, and minimal lock contention** on PostgreSQL, Oracle, SQL Server, or DB2 | **Extension-free cross-dialect SQL portability** or benchmarking `libpq` protocol overhead (`-M simple` vs. `-M prepared`) | **Benchmarking non-SQL engines** (Neo4j Cypher / ISO GQL, MongoDB MQL, Redis / DynamoDB / Key-Value stores) |
| **Network RTTs / Tx (Weighted)** | **`1.36` RTTs/tx** (`1` per `New-Order`, `Payment`, `Order-Status`, `Stock-Level`; `10` per `Delivery`) | **`29.57` RTTs/tx** (`47` per `New-Order`, `10.6` per `Payment`, `90` per `Delivery`) | **`1`–`10` RTTs/tx** (1 Cypher / MQL request per transaction or district step) |
| **Hot-Row Lock Hold Time (`warehouse`, `district`)** | **`~0.2–1.5 ms`** (pure in-memory SPI execution; `0` waits `>10 ms` per 1,000 txns) | **`Engine Time + 8..44 × Network RTT`** (`16+` `ShareLock` waits of `10–25 ms` per 1,000 txns even on loopback) | Engine-dependent (single-statement graph/document transaction) |
| **Connection Pooler / `-M simple` Efficiency** | **High** (`SPIPlanPtr` caches plans inside backend even when client sends `-M simple` via `PgBouncer`) | **Low in `-M simple`** (`3.5x–4.8x` slower due to repeated parsing/planning of 29.6 statements/tx); requires `-M prepared` | Protocol-dependent |
| **Parallel Data Load (`-s 4`, `-c 4 -j 4`)** | **`338.9 ms`** (`9` bulk `INSERT INTO ... SELECT ... FROM generate_series(...)` statements per worker) | **`2,317.3 ms`** (`~16,640` client-driven `INSERT`s across `\for` loops) | Client `\for` loops or native bulk import |
| **Server Prerequisites** | `CREATE PROCEDURE` (`PL/pgSQL`) + `contrib/pgbench` (`CREATE EXTENSION pgbench`) | Standard SQL tables + `BEGIN`/`COMMIT`/`ROLLBACK` (**zero extensions, zero stored procedures**) | Target query language processor (`--driver=neo4j`, document API, KV API) |

### 5.3 Complete Reference PostgreSQL Schema & `PL/pgSQL` Stored Procedures (`pgbench_order_schema.sql` & `pgbench_order_constraints.sql`)

```sql
SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS pgbench;

DROP TABLE IF EXISTS order_line CASCADE;
DROP TABLE IF EXISTS new_order CASCADE;
DROP TABLE IF EXISTS orders CASCADE;
DROP TABLE IF EXISTS history CASCADE;
DROP TABLE IF EXISTS customer CASCADE;
DROP TABLE IF EXISTS stock CASCADE;
DROP TABLE IF EXISTS item CASCADE;
DROP TABLE IF EXISTS district CASCADE;
DROP TABLE IF EXISTS warehouse CASCADE;

CREATE TABLE warehouse (
    w_id        integer PRIMARY KEY,
    w_name      varchar(10) NOT NULL,
    w_street_1  varchar(20) NOT NULL,
    w_street_2  varchar(20) NOT NULL,
    w_city      varchar(20) NOT NULL,
    w_state     char(2) NOT NULL,
    w_zip       char(9) NOT NULL,
    w_tax       numeric(4,4) NOT NULL,
    w_ytd       numeric(12,2) NOT NULL
);

CREATE TABLE district (
    d_id        smallint NOT NULL,
    d_w_id      integer NOT NULL,
    d_name      varchar(10) NOT NULL,
    d_street_1  varchar(20) NOT NULL,
    d_street_2  varchar(20) NOT NULL,
    d_city      varchar(20) NOT NULL,
    d_state     char(2) NOT NULL,
    d_zip       char(9) NOT NULL,
    d_tax       numeric(4,4) NOT NULL,
    d_ytd       numeric(12,2) NOT NULL,
    d_next_o_id integer NOT NULL,
    PRIMARY KEY (d_w_id, d_id)
);

CREATE TABLE customer (
    c_id           integer NOT NULL,
    c_d_id         smallint NOT NULL,
    c_w_id         integer NOT NULL,
    c_first        varchar(16) NOT NULL,
    c_middle       char(2) NOT NULL,
    c_last         varchar(16) NOT NULL,
    c_street_1     varchar(20) NOT NULL,
    c_street_2     varchar(20) NOT NULL,
    c_city         varchar(20) NOT NULL,
    c_state        char(2) NOT NULL,
    c_zip          char(9) NOT NULL,
    c_phone        char(16) NOT NULL,
    c_since        timestamp NOT NULL,
    c_credit       char(2) NOT NULL,
    c_credit_lim   numeric(12,2) NOT NULL,
    c_discount     numeric(4,4) NOT NULL,
    c_balance      numeric(12,2) NOT NULL,
    c_ytd_payment  numeric(12,2) NOT NULL,
    c_payment_cnt  integer NOT NULL,
    c_delivery_cnt integer NOT NULL,
    c_data         varchar(500) NOT NULL,
    PRIMARY KEY (c_w_id, c_d_id, c_id)
);

CREATE TABLE history (
    h_c_id   integer NOT NULL,
    h_c_d_id smallint NOT NULL,
    h_c_w_id integer NOT NULL,
    h_d_id   smallint NOT NULL,
    h_w_id   integer NOT NULL,
    h_date   timestamp NOT NULL,
    h_amount numeric(6,2) NOT NULL,
    h_data   varchar(24) NOT NULL
);

CREATE TABLE new_order (
    no_o_id integer NOT NULL,
    no_d_id smallint NOT NULL,
    no_w_id integer NOT NULL,
    PRIMARY KEY (no_w_id, no_d_id, no_o_id)
);

CREATE TABLE orders (
    o_id         integer NOT NULL,
    o_d_id       smallint NOT NULL,
    o_w_id       integer NOT NULL,
    o_c_id       integer NOT NULL,
    o_entry_d    timestamp NOT NULL,
    o_carrier_id smallint,
    o_ol_cnt     smallint NOT NULL,
    o_all_local  smallint NOT NULL,
    PRIMARY KEY (o_w_id, o_d_id, o_id)
);

CREATE TABLE order_line (
    ol_o_id        integer NOT NULL,
    ol_d_id        smallint NOT NULL,
    ol_w_id        integer NOT NULL,
    ol_number      smallint NOT NULL,
    ol_i_id        integer NOT NULL,
    ol_supply_w_id integer NOT NULL,
    ol_delivery_d  timestamp,
    ol_quantity    smallint NOT NULL,
    ol_amount      numeric(6,2) NOT NULL,
    ol_dist_info   char(24) NOT NULL,
    PRIMARY KEY (ol_w_id, ol_d_id, ol_o_id, ol_number)
);

CREATE TABLE item (
    i_id    integer PRIMARY KEY,
    i_im_id integer NOT NULL,
    i_name  varchar(24) NOT NULL,
    i_price numeric(5,2) NOT NULL,
    i_data  varchar(50) NOT NULL
);

CREATE TABLE stock (
    s_i_id       integer NOT NULL,
    s_w_id       integer NOT NULL,
    s_quantity   smallint NOT NULL,
    s_dist_01    char(24) NOT NULL,
    s_dist_02    char(24) NOT NULL,
    s_dist_03    char(24) NOT NULL,
    s_dist_04    char(24) NOT NULL,
    s_dist_05    char(24) NOT NULL,
    s_dist_06    char(24) NOT NULL,
    s_dist_07    char(24) NOT NULL,
    s_dist_08    char(24) NOT NULL,
    s_dist_09    char(24) NOT NULL,
    s_dist_10    char(24) NOT NULL,
    s_ytd        integer NOT NULL,
    s_order_cnt  integer NOT NULL,
    s_remote_cnt integer NOT NULL,
    s_data       varchar(50) NOT NULL,
    PRIMARY KEY (s_w_id, s_i_id)
);

-- 1. New-Order Stored Procedure
CREATE OR REPLACE PROCEDURE pgbench_order_new_order(
    p_w_id    integer,
    p_d_id    integer,
    p_c_id    integer,
    p_ol_cnt  integer,
    p_i_ids   integer[],
    p_s_w_ids integer[],
    p_qtys    integer[]
) LANGUAGE plpgsql AS $$
DECLARE
    v_w_tax      numeric(4,4);
    v_d_tax      numeric(4,4);
    v_o_id       integer;
    v_c_discount numeric(4,4);
    v_c_last     varchar(16);
    v_c_credit   char(2);
    v_all_local  smallint := 1;
    v_i_price    numeric(5,2);
    v_i_name     varchar(24);
    v_i_data     varchar(50);
    v_s_quantity smallint;
    v_s_data     varchar(50);
    v_dist_info  char(24);
    v_ol_amount  numeric(6,2);
    v_remote_inc integer;
    idx          integer;
BEGIN
    SELECT w_tax INTO v_w_tax
      FROM warehouse
     WHERE w_id = p_w_id;

    UPDATE district
       SET d_next_o_id = d_next_o_id + 1
     WHERE d_w_id = p_w_id AND d_id = p_d_id
    RETURNING d_next_o_id - 1, d_tax INTO v_o_id, v_d_tax;

    SELECT c_discount, c_last, c_credit
      INTO v_c_discount, v_c_last, v_c_credit
      FROM customer
     WHERE c_w_id = p_w_id AND c_d_id = p_d_id AND c_id = p_c_id;

    FOR idx IN 1 .. p_ol_cnt LOOP
        IF p_s_w_ids[idx] <> p_w_id THEN
            v_all_local := 0;
        END IF;
    END LOOP;

    INSERT INTO orders (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, o_carrier_id, o_ol_cnt, o_all_local)
    VALUES (v_o_id, p_d_id, p_w_id, p_c_id, CURRENT_TIMESTAMP, NULL, p_ol_cnt, v_all_local);

    INSERT INTO new_order (no_o_id, no_d_id, no_w_id)
    VALUES (v_o_id, p_d_id, p_w_id);

    FOR idx IN 1 .. p_ol_cnt LOOP
        SELECT i_price, i_name, i_data
          INTO v_i_price, v_i_name, v_i_data
          FROM item
         WHERE i_id = p_i_ids[idx];

        IF NOT FOUND THEN
            RAISE EXCEPTION 'Invalid item ID: %', p_i_ids[idx]
                USING ERRCODE = 'P0001';
        END IF;

        v_remote_inc := CASE WHEN p_s_w_ids[idx] <> p_w_id THEN 1 ELSE 0 END;

        UPDATE stock
           SET s_quantity = CASE
                                WHEN s_quantity - p_qtys[idx] >= 10
                                THEN s_quantity - p_qtys[idx]
                                ELSE s_quantity - p_qtys[idx] + 91
                            END,
               s_ytd = s_ytd + p_qtys[idx],
               s_order_cnt = s_order_cnt + 1,
               s_remote_cnt = s_remote_cnt + v_remote_inc
         WHERE s_w_id = p_s_w_ids[idx] AND s_i_id = p_i_ids[idx]
        RETURNING s_quantity, s_data,
                  CASE p_d_id
                      WHEN 1 THEN s_dist_01 WHEN 2 THEN s_dist_02 WHEN 3 THEN s_dist_03
                      WHEN 4 THEN s_dist_04 WHEN 5 THEN s_dist_05 WHEN 6 THEN s_dist_06
                      WHEN 7 THEN s_dist_07 WHEN 8 THEN s_dist_08 WHEN 9 THEN s_dist_09
                      ELSE s_dist_10
                  END
          INTO v_s_quantity, v_s_data, v_dist_info;

        v_ol_amount := round(p_qtys[idx] * v_i_price * (1 + v_w_tax + v_d_tax) * (1 - v_c_discount), 2);

        INSERT INTO order_line (
            ol_o_id, ol_d_id, ol_w_id, ol_number, ol_i_id,
            ol_supply_w_id, ol_delivery_d, ol_quantity, ol_amount, ol_dist_info
        ) VALUES (
            v_o_id, p_d_id, p_w_id, idx, p_i_ids[idx],
            p_s_w_ids[idx], NULL, p_qtys[idx], v_ol_amount, COALESCE(v_dist_info, 'DIST_INFO               ')
        );
    END LOOP;
END;
$$;

-- 2. Payment Stored Procedure
CREATE OR REPLACE PROCEDURE pgbench_order_payment(
    p_w_id     integer,
    p_d_id     integer,
    p_c_w_id   integer,
    p_c_d_id   integer,
    p_c_id     integer,
    p_c_last   text,
    p_h_amount numeric
) LANGUAGE plpgsql AS $$
DECLARE
    v_w_name   varchar(10);
    v_d_name   varchar(10);
    v_c_id     integer;
    v_c_credit char(2);
    v_c_data   varchar(500);
    v_cnt      integer;
BEGIN
    UPDATE warehouse
       SET w_ytd = w_ytd + p_h_amount
     WHERE w_id = p_w_id
    RETURNING w_name INTO v_w_name;

    UPDATE district
       SET d_ytd = d_ytd + p_h_amount
     WHERE d_w_id = p_w_id AND d_id = p_d_id
    RETURNING d_name INTO v_d_name;

    IF p_c_id IS NOT NULL THEN
        v_c_id := p_c_id;
    ELSE
        SELECT count(*) INTO v_cnt
          FROM customer
         WHERE c_w_id = p_c_w_id AND c_d_id = p_c_d_id AND c_last = p_c_last;

        IF v_cnt > 0 THEN
            SELECT c_id INTO v_c_id
              FROM customer
             WHERE c_w_id = p_c_w_id AND c_d_id = p_c_d_id AND c_last = p_c_last
             ORDER BY c_first
            OFFSET (v_cnt - 1) / 2 LIMIT 1;
        ELSE
            SELECT c_id INTO v_c_id
              FROM customer
             WHERE c_w_id = p_c_w_id AND c_d_id = p_c_d_id
             ORDER BY c_last >= p_c_last DESC, c_last, c_first
             LIMIT 1;
        END IF;
    END IF;

    SELECT c_credit, c_data
      INTO v_c_credit, v_c_data
      FROM customer
     WHERE c_w_id = p_c_w_id AND c_d_id = p_c_d_id AND c_id = v_c_id
       FOR UPDATE;

    IF v_c_credit = 'BC' THEN
        v_c_data := left(pgbench.format_string('%d %d %d %d %d %.2f | %s',
                         v_c_id, p_c_d_id, p_c_w_id, p_d_id, p_w_id, p_h_amount, v_c_data), 500);
        UPDATE customer
           SET c_balance = c_balance - p_h_amount,
               c_ytd_payment = c_ytd_payment + p_h_amount,
               c_payment_cnt = c_payment_cnt + 1,
               c_data = v_c_data
         WHERE c_w_id = p_c_w_id AND c_d_id = p_c_d_id AND c_id = v_c_id;
    ELSE
        UPDATE customer
           SET c_balance = c_balance - p_h_amount,
               c_ytd_payment = c_ytd_payment + p_h_amount,
               c_payment_cnt = c_payment_cnt + 1
         WHERE c_w_id = p_c_w_id AND c_d_id = p_c_d_id AND c_id = v_c_id;
    END IF;

    INSERT INTO history (h_c_id, h_c_d_id, h_c_w_id, h_d_id, h_w_id, h_date, h_amount, h_data)
    VALUES (v_c_id, p_c_d_id, p_c_w_id, p_d_id, p_w_id, CURRENT_TIMESTAMP,
            p_h_amount, left(v_w_name || '    ' || v_d_name, 24));
END;
$$;

-- 3. Order-Status Stored Procedure
CREATE OR REPLACE PROCEDURE pgbench_order_order_status(
    p_w_id   integer,
    p_d_id   integer,
    p_c_id   integer,
    p_c_last text
) LANGUAGE plpgsql AS $$
DECLARE
    v_c_id       integer;
    v_c_balance  numeric(12,2);
    v_c_first    varchar(16);
    v_c_middle   char(2);
    v_c_last     varchar(16);
    v_o_id       integer;
    v_o_entry_d  timestamp;
    v_o_carrier  smallint;
    v_ol_cnt     integer;
    v_cnt        integer;
BEGIN
    IF p_c_id IS NOT NULL THEN
        v_c_id := p_c_id;
    ELSE
        SELECT count(*) INTO v_cnt
          FROM customer
         WHERE c_w_id = p_w_id AND c_d_id = p_d_id AND c_last = p_c_last;

        IF v_cnt > 0 THEN
            SELECT c_id INTO v_c_id
              FROM customer
             WHERE c_w_id = p_w_id AND c_d_id = p_d_id AND c_last = p_c_last
             ORDER BY c_first
            OFFSET (v_cnt - 1) / 2 LIMIT 1;
        ELSE
            SELECT c_id INTO v_c_id
              FROM customer
             WHERE c_w_id = p_w_id AND c_d_id = p_d_id
             ORDER BY c_last >= p_c_last DESC, c_last, c_first
             LIMIT 1;
        END IF;
    END IF;

    SELECT c_balance, c_first, c_middle, c_last
      INTO v_c_balance, v_c_first, v_c_middle, v_c_last
      FROM customer
     WHERE c_w_id = p_w_id AND c_d_id = p_d_id AND c_id = v_c_id;

    SELECT o_id, o_entry_d, o_carrier_id
      INTO v_o_id, v_o_entry_d, v_o_carrier
      FROM orders
     WHERE o_w_id = p_w_id AND o_d_id = p_d_id AND o_c_id = v_c_id
     ORDER BY o_id DESC
     LIMIT 1;

    IF v_o_id IS NOT NULL THEN
        SELECT count(*) INTO v_ol_cnt
          FROM order_line
         WHERE ol_w_id = p_w_id AND ol_d_id = p_d_id AND ol_o_id = v_o_id;
    END IF;
END;
$$;

-- 4. Delivery Stored Procedure (single district step)
CREATE OR REPLACE PROCEDURE pgbench_order_delivery(
    p_w_id         integer,
    p_d_id         integer,
    p_o_carrier_id integer
) LANGUAGE plpgsql AS $$
DECLARE
    v_o_id     integer;
    v_c_id     integer;
    v_ol_total numeric(12,2);
BEGIN
    DELETE FROM new_order
     WHERE no_w_id = p_w_id
       AND no_d_id = p_d_id
       AND no_o_id = (
           SELECT min(no_o_id)
             FROM new_order
            WHERE no_w_id = p_w_id AND no_d_id = p_d_id
       )
    RETURNING no_o_id INTO v_o_id;

    IF v_o_id IS NOT NULL THEN
        UPDATE orders
           SET o_carrier_id = p_o_carrier_id
         WHERE o_w_id = p_w_id AND o_d_id = p_d_id AND o_id = v_o_id
        RETURNING o_c_id INTO v_c_id;

        UPDATE order_line
           SET ol_delivery_d = CURRENT_TIMESTAMP
         WHERE ol_w_id = p_w_id AND ol_d_id = p_d_id AND ol_o_id = v_o_id;

        SELECT COALESCE(sum(ol_amount), 0)
          INTO v_ol_total
          FROM order_line
         WHERE ol_w_id = p_w_id AND ol_d_id = p_d_id AND ol_o_id = v_o_id;

        UPDATE customer
           SET c_balance = c_balance + v_ol_total,
               c_delivery_cnt = c_delivery_cnt + 1
         WHERE c_w_id = p_w_id AND c_d_id = p_d_id AND c_id = v_c_id;
    END IF;
END;
$$;

-- 5. Stock-Level Stored Procedure
CREATE OR REPLACE PROCEDURE pgbench_order_stock_level(
    p_w_id      integer,
    p_d_id      integer,
    p_threshold integer,
    INOUT p_low_stock integer DEFAULT 0
) LANGUAGE plpgsql AS $$
DECLARE
    v_next_o_id integer;
BEGIN
    SELECT d_next_o_id INTO v_next_o_id
      FROM district
     WHERE d_w_id = p_w_id AND d_id = p_d_id;

    SELECT count(DISTINCT s_i_id)
      INTO p_low_stock
      FROM order_line
      JOIN stock ON s_w_id = ol_w_id AND s_i_id = ol_i_id
     WHERE ol_w_id = p_w_id
       AND ol_d_id = p_d_id
       AND ol_o_id >= v_next_o_id - 20
       AND ol_o_id < v_next_o_id
       AND s_quantity < p_threshold;
END;
$$;
```

Post-load secondary indexes (`pgbench_order_constraints.sql` / `pgbench_order_constraints.sql`):
```sql
CREATE INDEX IF NOT EXISTS idx_customer_name
    ON customer (c_w_id, c_d_id, c_last, c_first);

CREATE INDEX IF NOT EXISTS idx_orders_customer
    ON orders (o_w_id, o_d_id, o_c_id, o_id DESC);

ANALYZE warehouse, district, customer, history, new_order, orders, order_line, item, stock;
```

### 5.4 Server-Side Parallel Data Generator Using `contrib/pgbench` C Functions (`pgbench_order_init.sql` / `pgbench_order_init.sql`)

```sql
SELECT
    COALESCE(substring(':num_workers' FROM '^[0-9]+$')::int, 1) AS num_workers,
    COALESCE(substring(':items_per_scale' FROM '^[0-9]+$')::int, 1000) AS items_per_scale,
    COALESCE(substring(':cust_per_dist' FROM '^[0-9]+$')::int, 30) AS cust_per_dist
\gset

SELECT pgbench.setseed((:random_seed + :client_id)::bigint);

-- 1. Item table (partitioned across workers by i_id)
INSERT INTO item (i_id, i_im_id, i_name, i_price, i_data)
SELECT
    i_id,
    pgbench.random(1, 10000)::int,
    pgbench.format_string('ITEM-%06d-%s', i_id, pgbench.random_astring(4, 10)),
    (pgbench.random(100, 10000) / 100.0)::numeric(5,2),
    CASE WHEN pgbench.random(1, 10) = 1
         THEN pgbench.random_astring(10, 20) || 'ORIGINAL' || pgbench.random_astring(8, 12)
         ELSE pgbench.random_astring(26, 50)
    END
FROM generate_series(:client_id + 1, :items_per_scale, :num_workers) AS g(i_id);

-- 2. Warehouse table (partitioned across workers by w_id)
INSERT INTO warehouse (w_id, w_name, w_street_1, w_street_2, w_city, w_state, w_zip, w_tax, w_ytd)
SELECT
    w_id,
    pgbench.format_string('WH-%04d', w_id),
    pgbench.random_astring(10, 20),
    pgbench.random_astring(10, 20),
    pgbench.random_astring(10, 20),
    chr(65 + (w_id % 25)) || chr(65 + ((w_id / 25) % 25)),
    pgbench.random_nstring(4, 4) || '11111',
    (pgbench.random(0, 2000) / 10000.0)::numeric(4,4),
    300000.00
FROM generate_series(:client_id + 1, :scale, :num_workers) AS g(w_id);

-- 3. Stock table (partitioned across workers by s_i_id)
INSERT INTO stock (
    s_i_id, s_w_id, s_quantity,
    s_dist_01, s_dist_02, s_dist_03, s_dist_04, s_dist_05,
    s_dist_06, s_dist_07, s_dist_08, s_dist_09, s_dist_10,
    s_ytd, s_order_cnt, s_remote_cnt, s_data
)
SELECT
    i_id, w_id, pgbench.random(10, 100)::smallint,
    pgbench.random_astring(24, 24), pgbench.random_astring(24, 24),
    pgbench.random_astring(24, 24), pgbench.random_astring(24, 24),
    pgbench.random_astring(24, 24), pgbench.random_astring(24, 24),
    pgbench.random_astring(24, 24), pgbench.random_astring(24, 24),
    pgbench.random_astring(24, 24), pgbench.random_astring(24, 24),
    0, 0, 0,
    CASE WHEN pgbench.random(1, 10) = 1
         THEN pgbench.random_astring(10, 20) || 'ORIGINAL' || pgbench.random_astring(8, 12)
         ELSE pgbench.random_astring(26, 50)
    END
FROM generate_series(1, :scale) AS w(w_id)
CROSS JOIN generate_series(:client_id + 1, :items_per_scale, :num_workers) AS i(i_id);

-- 4. District table (partitioned across workers by (w_id, d_id) slice)
INSERT INTO district (
    d_id, d_w_id, d_name, d_street_1, d_street_2, d_city, d_state, d_zip,
    d_tax, d_ytd, d_next_o_id
)
SELECT
    d_id::smallint, w_id,
    pgbench.format_string('DIST-%02d-%02d', w_id, d_id),
    pgbench.random_astring(10, 20), pgbench.random_astring(10, 20), pgbench.random_astring(10, 20),
    chr(65 + ((w_id + d_id) % 25)) || chr(65 + (d_id % 25)),
    pgbench.random_nstring(4, 4) || '11111',
    (pgbench.random(0, 2000) / 10000.0)::numeric(4,4),
    30000.00, :cust_per_dist + 1
FROM generate_series(1, :scale) AS w(w_id)
CROSS JOIN generate_series(1, 10) AS d(d_id)
WHERE ((w_id - 1) * 10 + (d_id - 1)) % :num_workers = :client_id;

-- 5. Customer table (partitioned across workers by (w_id, d_id) slice)
INSERT INTO customer (
    c_id, c_d_id, c_w_id, c_first, c_middle, c_last,
    c_street_1, c_street_2, c_city, c_state, c_zip, c_phone,
    c_since, c_credit, c_credit_lim, c_discount,
    c_balance, c_ytd_payment, c_payment_cnt, c_delivery_cnt, c_data
)
SELECT
    c_id, d_id::smallint, w_id,
    pgbench.random_astring(8, 16), 'OE',
    CASE WHEN c_id <= 1000
         THEN pgbench.c_last((c_id - 1) % 1000)
         ELSE pgbench.c_last(pgbench.nurand(255, 0, 999))
    END,
    pgbench.random_astring(10, 20), pgbench.random_astring(10, 20), pgbench.random_astring(10, 20),
    chr(65 + ((c_id + d_id + w_id) % 25)) || chr(65 + (c_id % 25)),
    pgbench.random_nstring(4, 4) || '11111',
    pgbench.random_nstring(16, 16),
    pgbench.date_add('2020-01-01', c_id % 365, 'day')::timestamp,
    CASE WHEN pgbench.random(1, 10) = 1 THEN 'BC' ELSE 'GC' END,
    50000.00, (pgbench.random(0, 5000) / 10000.0)::numeric(4,4),
    -10.00, 10.00, 1, 0, pgbench.random_astring(30, 60)
FROM generate_series(1, :scale) AS w(w_id)
CROSS JOIN generate_series(1, 10) AS d(d_id)
CROSS JOIN generate_series(1, :cust_per_dist) AS c(c_id)
WHERE ((w_id - 1) * 10 + (d_id - 1)) % :num_workers = :client_id;

-- 6. History table (partitioned across workers by (w_id, d_id) slice)
INSERT INTO history (h_c_id, h_c_d_id, h_c_w_id, h_d_id, h_w_id, h_date, h_amount, h_data)
SELECT
    c_id, d_id::smallint, w_id, d_id::smallint, w_id,
    pgbench.date_add('2020-01-01', c_id % 365, 'day')::timestamp,
    10.00, pgbench.random_astring(12, 24)
FROM generate_series(1, :scale) AS w(w_id)
CROSS JOIN generate_series(1, 10) AS d(d_id)
CROSS JOIN generate_series(1, :cust_per_dist) AS c(c_id)
WHERE ((w_id - 1) * 10 + (d_id - 1)) % :num_workers = :client_id;

-- 7. Orders table (partitioned across workers by (w_id, d_id) slice)
INSERT INTO orders (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, o_carrier_id, o_ol_cnt, o_all_local)
SELECT
    o_id, d_id::smallint, w_id,
    (pgbench.permute(o_id - 1, :cust_per_dist, w_id * 100 + d_id) + 1)::int,
    pgbench.date_add('2020-01-01', o_id % 365, 'day')::timestamp,
    CASE WHEN o_id <= (:cust_per_dist * 7) / 10
         THEN pgbench.random(1, 10)::smallint
         ELSE NULL
    END,
    (5 + ((o_id + d_id + w_id) % 6))::smallint, 1
FROM generate_series(1, :scale) AS w(w_id)
CROSS JOIN generate_series(1, 10) AS d(d_id)
CROSS JOIN generate_series(1, :cust_per_dist) AS o(o_id)
WHERE ((w_id - 1) * 10 + (d_id - 1)) % :num_workers = :client_id;

-- 8. New-Order table (last 30% of orders per district)
INSERT INTO new_order (no_o_id, no_d_id, no_w_id)
SELECT o_id, d_id::smallint, w_id
FROM generate_series(1, :scale) AS w(w_id)
CROSS JOIN generate_series(1, 10) AS d(d_id)
CROSS JOIN generate_series(((:cust_per_dist * 7) / 10) + 1, :cust_per_dist) AS o(o_id)
WHERE ((w_id - 1) * 10 + (d_id - 1)) % :num_workers = :client_id;

-- 9. Order-Line table (partitioned across workers by (w_id, d_id) slice)
INSERT INTO order_line (
    ol_o_id, ol_d_id, ol_w_id, ol_number, ol_i_id,
    ol_supply_w_id, ol_delivery_d, ol_quantity, ol_amount, ol_dist_info
)
SELECT
    o_id, d_id::smallint, w_id, ol_num::smallint,
    pgbench.random(1, :items_per_scale)::int, w_id,
    CASE WHEN o_id <= (:cust_per_dist * 7) / 10
         THEN pgbench.date_add('2020-01-02', o_id % 365, 'day')::timestamp
         ELSE NULL
    END,
    5,
    CASE WHEN o_id <= (:cust_per_dist * 7) / 10
         THEN 0.00
         ELSE (pgbench.random(10, 5000) / 100.0)::numeric(6,2)
    END,
    pgbench.random_astring(24, 24)
FROM generate_series(1, :scale) AS w(w_id)
CROSS JOIN generate_series(1, 10) AS d(d_id)
CROSS JOIN generate_series(1, :cust_per_dist) AS o(o_id)
CROSS JOIN LATERAL generate_series(1, 5 + ((o_id + d_id + w_id) % 6)) AS l(ol_num)
WHERE ((w_id - 1) * 10 + (d_id - 1)) % :num_workers = :client_id;
```

### 5.5 Thin `pgbench` Client Transaction Scripts (`CALL pgbench_order_*(...)`) & Manifest (`pgbench_order.pgbench.json`)

#### 1. `pgbench_order_new_order.sql` (`pgbench_order_new_order.sql`, Weight `45`)
```sql
\keying_time 1 ms
\set w_id random(1, :scale)
\set d_id random(1, 10)
\set c_id nurand(1023, 1, 30)
\set ol_cnt random(5, 15)
\set rbk random(1, 100)

\set i_ids array_generate(:ol_cnt, 1)
\set s_w_ids array_generate(:ol_cnt, :w_id)
\set qtys array_generate(:ol_cnt, 5)

\for idx 0 .. (:ol_cnt - 1) BY 1
  \set i_id nurand(8191, 1, 1000)
  \set i_ids[:idx] :i_id
  \set s_w_ids[:idx] :w_id
  \set qtys[:idx] random(1, 10)
  \if :scale > 1 AND random(1, 100) = 1
    \set r_w random(1, :scale - 1)
    \set s_w_ids[:idx] CASE WHEN :r_w >= :w_id THEN :r_w + 1 ELSE :r_w END
  \endif
\endfor

\if :rbk = 1
  \set i_ids[:ol_cnt - 1] -1
\endif

\try
CALL pgbench_order_new_order(:w_id, :d_id, :c_id, :ol_cnt, :i_ids, :s_w_ids, :qtys);
\catch 'P0001'
\endtry
\think_time random_exponential(1, 5, 2.0) ms
```

#### 2. `pgbench_order_payment.sql` (`pgbench_order_payment.sql`, Weight `43`)
```sql
\keying_time 1 ms
\set w_id random(1, :scale)
\set d_id random(1, 10)
\set h_amount random(100, 500000) / 100.0

\if :scale > 1 AND random(1, 100) <= 15
  \set r_w random(1, :scale - 1)
  \set c_w_id CASE WHEN :r_w >= :w_id THEN :r_w + 1 ELSE :r_w END
  \set c_d_id random(1, 10)
\else
  \set c_w_id :w_id
  \set c_d_id :d_id
\endif

\if random(1, 100) <= 60
  \set c_last c_last(nurand(255, 0, 999))
  CALL pgbench_order_payment(:w_id, :d_id, :c_w_id, :c_d_id, NULL, :c_last, :h_amount);
\else
  \set c_id nurand(1023, 1, 30)
  CALL pgbench_order_payment(:w_id, :d_id, :c_w_id, :c_d_id, :c_id, NULL, :h_amount);
\endif
\think_time random_exponential(1, 5, 2.0) ms
```

#### 3. `pgbench_order_order_status.sql` (`pgbench_order_order_status.sql`, Weight `4`)
```sql
\keying_time 1 ms
\set w_id random(1, :scale)
\set d_id random(1, 10)

\if random(1, 100) <= 60
  \set c_last c_last(nurand(255, 0, 999))
  CALL pgbench_order_order_status(:w_id, :d_id, NULL, :c_last);
\else
  \set c_id nurand(1023, 1, 30)
  CALL pgbench_order_order_status(:w_id, :d_id, :c_id, NULL);
\endif
\think_time random_exponential(1, 5, 2.0) ms
```

#### 4. `pgbench_order_delivery.sql` (`pgbench_order_delivery.sql`, Weight `4`)
```sql
\keying_time 1 ms
\set w_id random(1, :scale)
\set o_carrier_id random(1, 10)

\for d_id 1 .. 10 BY 1
  CALL pgbench_order_delivery(:w_id, :d_id, :o_carrier_id);
\endfor
\think_time random_exponential(1, 5, 2.0) ms
```

#### 5. `pgbench_order_stock_level.sql` (`pgbench_order_stock_level.sql`, Weight `4`)
```sql
\keying_time 1 ms
\set w_id random(1, :scale)
\set d_id random(1, 10)
\set threshold random(10, 20)

CALL pgbench_order_stock_level(:w_id, :d_id, :threshold, 0);
\think_time random_exponential(1, 5, 2.0) ms
```

#### 6. Declarative Manifest (`pgbench_order.pgbench.json` / `pgbench_c.pgbench.json`)
> **Tip (`--max-tries=10` / `"max_tries": 10`)**: Pass `--max-tries=10` on the CLI (or add `"max_tries": 10` to `.pgbench.json`) when running with multiple concurrent clients so any `40P01` (`deadlock_detected`) on overlapping `stock(s_w_id, s_i_id)` updates is transparently retried (`$.overall.transactions_failed == 0`).

```json
{
  "suite": "pgbench-order",
  "description": "Wholesale order-entry OLTP workload (New-Order 45%, Payment 43%, Order-Status 4%, Delivery 4%, Stock-Level 4%)",
  "scale": 4,
  "no_vacuum": true,
  "clients": 16,
  "threads": 4,
  "duration_sec": 60,
  "init_steps": [
    { "name": "schema", "file": "pgbench_order_schema.sql" },
    { "name": "data", "file": "pgbench_order_init.sql", "clients": 4, "threads": 4, "variables": { "num_workers": 4 } },
    { "name": "constraints", "file": "pgbench_order_constraints.sql" }
  ],
  "scripts": [
    { "file": "pgbench_order_new_order.sql", "weight": 45 },
    { "file": "pgbench_order_payment.sql", "weight": 43 },
    { "file": "pgbench_order_order_status.sql", "weight": 4 },
    { "file": "pgbench_order_delivery.sql", "weight": 4 },
    { "file": "pgbench_order_stock_level.sql", "weight": 4 }
  ],
  "sla": [
    "total:p95<=5000ms"
  ]
}
```

---

## 6. Pure Client-Driven SQL DML Templates & Scripting Rules (`Model B` — No Stored Procedures, No `contrib/pgbench`)

Use the pure client-driven scripts below (also in `doc/agent-guide-pgbench-order.md` Section 6) when porting to any SQL engine without `contrib/pgbench` or PL/pgSQL procedures, and obey these 4 critical rules:
1. **Unquoted `:var` in `pgbench` Meta-Commands**: Never single-quote variables inside `\if`/`\elif`/`\while`/`\set` expressions (write `\if :c_credit = 'BC'`, never `\if ':c_credit' = 'BC'`, because `exprscan.l` lexes `':c_credit'` as the literal string `":c_credit"`).
2. **Unquoted `:var` in SQL Statements (`-M simple` / `-M extended` / `-M prepared`)**: `assignVariables()` automatically single-quotes and SQL-escapes `PGBT_TEXT`/`PGBT_TIMESTAMP` variables (`:c_last`, `:new_c_data`, `:h_data`) in `-M simple` mode, while `parseQuery()` binds them as `$N` parameters in `-M extended`/`-M prepared` mode. Single-quoting `':var'` in SQL breaks `-M extended` and `-M prepared`.
3. **Scoped `\catch '22012'` in `New-Order`**: Inside `\try ... \catch '22012'`, execute:
   ```sql
   SELECT 1 / count(*) AS item_ok, max(i_price) AS i_price
     FROM item WHERE i_id = :cur_i_id \gset
   ```
   When `:cur_i_id = -1`, `count(*) = 0` triggers `SQLSTATE '22012'` (`division_by_zero`), unwinding the `\for` loop, rolling back the transaction block, and incrementing `$.overall.expected_rollbacks`. Scoping `\catch '22012'` (instead of bare `\catch`) ensures `40001`/`40P01` concurrency conflicts are retried by `--max-tries` rather than swallowed as expected rollbacks.
4. **Per-District Locking in `Delivery`**: Lock `district` (`SELECT d_id AS locked_d_id FROM district WHERE d_w_id = :w_id AND d_id = :d_id FOR UPDATE \gset`) before `SELECT COALESCE(min(no_o_id), 0)` in `delivery_client.sql` to prevent concurrent double-delivery.

### 6.1 Pure Client-Driven `new_order_client.sql` (Weight `45`)
```sql
\keying_time 1 ms
\set w_id random(1, :scale)
\set d_id random(1, 10)
\set c_id nurand(1023, 1, 30)
\set ol_cnt random(5, 15)
\set rbk random(1, 100)

\set i_ids array_generate(:ol_cnt, 1)
\set s_w_ids array_generate(:ol_cnt, :w_id)
\set qtys array_generate(:ol_cnt, 5)
\set all_local 1

\for idx 0 .. (:ol_cnt - 1) BY 1
  \set i_ids[:idx] nurand(8191, 1, 1000)
  \set s_w_ids[:idx] :w_id
  \set qtys[:idx] random(1, 10)
  \if :scale > 1 AND random(1, 100) = 1
    \set r_w random(1, :scale - 1)
    \set s_w_ids[:idx] CASE WHEN :r_w >= :w_id THEN :r_w + 1 ELSE :r_w END
    \set all_local 0
  \endif
\endfor

\if :rbk = 1
  \set i_ids[:ol_cnt - 1] -1
\endif

\try
BEGIN;

SELECT w_tax FROM warehouse WHERE w_id = :w_id \gset
SELECT d_next_o_id AS v_o_id, d_tax FROM district WHERE d_w_id = :w_id AND d_id = :d_id FOR UPDATE \gset
UPDATE district SET d_next_o_id = d_next_o_id + 1 WHERE d_w_id = :w_id AND d_id = :d_id;
SELECT c_discount, c_last, c_credit FROM customer WHERE c_w_id = :w_id AND c_d_id = :d_id AND c_id = :c_id \gset

INSERT INTO orders (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, o_carrier_id, o_ol_cnt, o_all_local)
VALUES (:v_o_id, :d_id, :w_id, :c_id, CURRENT_TIMESTAMP, NULL, :ol_cnt, :all_local);

INSERT INTO new_order (no_o_id, no_d_id, no_w_id)
VALUES (:v_o_id, :d_id, :w_id);

\for idx 0 .. (:ol_cnt - 1) BY 1
  \set cur_i_id :i_ids[:idx]
  \set cur_s_w_id :s_w_ids[:idx]
  \set cur_qty :qtys[:idx]
  \set ol_num (:idx + 1)
  \set remote_inc CASE WHEN :cur_s_w_id <> :w_id THEN 1 ELSE 0 END

  SELECT 1 / count(*) AS item_ok, max(i_price) AS i_price
    FROM item WHERE i_id = :cur_i_id \gset

  SELECT s_quantity AS cur_s_qty,
         CASE
           WHEN :d_id = 1 THEN s_dist_01 WHEN :d_id = 2 THEN s_dist_02 WHEN :d_id = 3 THEN s_dist_03
           WHEN :d_id = 4 THEN s_dist_04 WHEN :d_id = 5 THEN s_dist_05 WHEN :d_id = 6 THEN s_dist_06
           WHEN :d_id = 7 THEN s_dist_07 WHEN :d_id = 8 THEN s_dist_08 WHEN :d_id = 9 THEN s_dist_09
           ELSE s_dist_10
         END AS v_dist_info
    FROM stock WHERE s_w_id = :cur_s_w_id AND s_i_id = :cur_i_id FOR UPDATE \gset

  \set new_s_qty CASE WHEN :cur_s_qty - :cur_qty >= 10 THEN :cur_s_qty - :cur_qty ELSE :cur_s_qty - :cur_qty + 91 END
  \set v_dist_str format_string('%s', :v_dist_info)
  \set ol_amt format_string('%.2f', :cur_qty * :i_price * (1.0 + :w_tax + :d_tax) * (1.0 - :c_discount)) + 0.0

  UPDATE stock
     SET s_quantity = :new_s_qty,
         s_ytd = s_ytd + :cur_qty,
         s_order_cnt = s_order_cnt + 1,
         s_remote_cnt = s_remote_cnt + :remote_inc
   WHERE s_w_id = :cur_s_w_id AND s_i_id = :cur_i_id;

  INSERT INTO order_line (
    ol_o_id, ol_d_id, ol_w_id, ol_number, ol_i_id,
    ol_supply_w_id, ol_delivery_d, ol_quantity, ol_amount, ol_dist_info
  ) VALUES (
    :v_o_id, :d_id, :w_id, :ol_num, :cur_i_id,
    :cur_s_w_id, NULL, :cur_qty, :ol_amt, :v_dist_str
  );
\endfor

COMMIT;
\catch '22012'
ROLLBACK;
\endtry
\think_time random_exponential(1, 5, 2.0) ms
```

### 6.2 Pure Client-Driven `payment_client.sql` (Weight `43`)
```sql
\keying_time 1 ms
\set w_id random(1, :scale)
\set d_id random(1, 10)
\set h_amount random(100, 500000) / 100.0

\if :scale > 1 AND random(1, 100) <= 15
  \set r_w random(1, :scale - 1)
  \set c_w_id CASE WHEN :r_w >= :w_id THEN :r_w + 1 ELSE :r_w END
  \set c_d_id random(1, 10)
\else
  \set c_w_id :w_id
  \set c_d_id :d_id
\endif

BEGIN;

UPDATE warehouse SET w_ytd = w_ytd + :h_amount WHERE w_id = :w_id;
SELECT w_name FROM warehouse WHERE w_id = :w_id \gset

UPDATE district SET d_ytd = d_ytd + :h_amount WHERE d_w_id = :w_id AND d_id = :d_id;
SELECT d_name FROM district WHERE d_w_id = :w_id AND d_id = :d_id \gset

\if random(1, 100) <= 60
  \set c_last c_last(nurand(255, 0, 999))
  SELECT count(*) AS v_cnt FROM customer
   WHERE c_w_id = :c_w_id AND c_d_id = :c_d_id AND c_last = :c_last \gset
  \if :v_cnt > 0
    \set mid_offset (:v_cnt - 1) / 2
    SELECT c_id AS v_c_id FROM customer
     WHERE c_w_id = :c_w_id AND c_d_id = :c_d_id AND c_last = :c_last
     ORDER BY c_first ASC LIMIT 1 OFFSET :mid_offset \gset
  \else
    SELECT c_id AS v_c_id FROM customer
     WHERE c_w_id = :c_w_id AND c_d_id = :c_d_id
     ORDER BY CASE WHEN c_last >= :c_last THEN 0 ELSE 1 END, c_last ASC, c_first ASC
     LIMIT 1 \gset
  \endif
\else
  \set v_c_id nurand(1023, 1, 30)
\endif

SELECT c_credit, c_data FROM customer
 WHERE c_w_id = :c_w_id AND c_d_id = :c_d_id AND c_id = :v_c_id FOR UPDATE \gset

\if :c_credit = 'BC'
  \set new_c_data format_string('%d %d %d %d %d %.2f | %s', :v_c_id, :c_d_id, :c_w_id, :d_id, :w_id, :h_amount, :c_data)
  UPDATE customer
     SET c_balance = c_balance - :h_amount,
         c_ytd_payment = c_ytd_payment + :h_amount,
         c_payment_cnt = c_payment_cnt + 1,
         c_data = left(:new_c_data, 500)
   WHERE c_w_id = :c_w_id AND c_d_id = :c_d_id AND c_id = :v_c_id;
\else
  UPDATE customer
     SET c_balance = c_balance - :h_amount,
         c_ytd_payment = c_ytd_payment + :h_amount,
         c_payment_cnt = c_payment_cnt + 1
   WHERE c_w_id = :c_w_id AND c_d_id = :c_d_id AND c_id = :v_c_id;
\endif

\set h_data format_string('%s    %s', :w_name, :d_name)
INSERT INTO history (h_c_id, h_c_d_id, h_c_w_id, h_d_id, h_w_id, h_date, h_amount, h_data)
VALUES (:v_c_id, :c_d_id, :c_w_id, :d_id, :w_id, CURRENT_TIMESTAMP, :h_amount, left(:h_data, 24));

COMMIT;
\think_time random_exponential(1, 5, 2.0) ms
```

### 6.3 Pure Client-Driven `order_status_client.sql`, `delivery_client.sql`, and `stock_level_client.sql` (Weight `4` Each)
```sql
-- order_status_client.sql (Weight 4):
\keying_time 1 ms
\set w_id random(1, :scale)
\set d_id random(1, 10)
\if random(1, 100) <= 60
  \set c_last c_last(nurand(255, 0, 999))
  SELECT count(*) AS v_cnt FROM customer
   WHERE c_w_id = :w_id AND c_d_id = :d_id AND c_last = :c_last \gset
  \if :v_cnt > 0
    \set mid_offset (:v_cnt - 1) / 2
    SELECT c_id AS v_c_id FROM customer
     WHERE c_w_id = :w_id AND c_d_id = :d_id AND c_last = :c_last
     ORDER BY c_first ASC LIMIT 1 OFFSET :mid_offset \gset
  \else
    SELECT c_id AS v_c_id FROM customer
     WHERE c_w_id = :w_id AND c_d_id = :d_id
     ORDER BY CASE WHEN c_last >= :c_last THEN 0 ELSE 1 END, c_last ASC, c_first ASC
     LIMIT 1 \gset
  \endif
\else
  \set v_c_id nurand(1023, 1, 30)
\endif
SELECT c_balance, c_first, c_middle, c_last FROM customer
 WHERE c_w_id = :w_id AND c_d_id = :d_id AND c_id = :v_c_id \gset
SELECT COALESCE(max(o_id), 0) AS v_o_id FROM orders
 WHERE o_w_id = :w_id AND o_d_id = :d_id AND o_c_id = :v_c_id \gset
\if :v_o_id > 0
  SELECT count(*) AS v_ol_cnt FROM order_line
   WHERE ol_w_id = :w_id AND ol_d_id = :d_id AND ol_o_id = :v_o_id \gset
\endif
\think_time random_exponential(1, 5, 2.0) ms
```

```sql
-- delivery_client.sql (Weight 4):
\keying_time 1 ms
\set w_id random(1, :scale)
\set o_carrier_id random(1, 10)
\for d_id 1 .. 10 BY 1
  BEGIN;
  SELECT d_id AS locked_d_id
    FROM district WHERE d_w_id = :w_id AND d_id = :d_id FOR UPDATE \gset
  SELECT COALESCE(min(no_o_id), 0) AS v_o_id
    FROM new_order WHERE no_w_id = :w_id AND no_d_id = :d_id \gset
  \if :v_o_id > 0
    DELETE FROM new_order
     WHERE no_w_id = :w_id AND no_d_id = :d_id AND no_o_id = :v_o_id;
    UPDATE orders SET o_carrier_id = :o_carrier_id
     WHERE o_w_id = :w_id AND o_d_id = :d_id AND o_id = :v_o_id;
    SELECT o_c_id AS v_c_id FROM orders
     WHERE o_w_id = :w_id AND o_d_id = :d_id AND o_id = :v_o_id \gset
    UPDATE order_line SET ol_delivery_d = CURRENT_TIMESTAMP
     WHERE ol_w_id = :w_id AND ol_d_id = :d_id AND ol_o_id = :v_o_id;
    SELECT COALESCE(sum(ol_amount), 0) AS v_ol_total FROM order_line
     WHERE ol_w_id = :w_id AND ol_d_id = :d_id AND ol_o_id = :v_o_id \gset
    UPDATE customer
       SET c_balance = c_balance + :v_ol_total,
           c_delivery_cnt = c_delivery_cnt + 1
     WHERE c_w_id = :w_id AND c_d_id = :d_id AND c_id = :v_c_id;
  \endif
  COMMIT;
\endfor
\think_time random_exponential(1, 5, 2.0) ms
```

```sql
-- stock_level_client.sql (Weight 4):
\keying_time 1 ms
\set w_id random(1, :scale)
\set d_id random(1, 10)
\set threshold random(10, 20)
SELECT d_next_o_id AS v_next_o_id FROM district
 WHERE d_w_id = :w_id AND d_id = :d_id \gset
\set min_o_id (:v_next_o_id - 20)
SELECT count(DISTINCT s_i_id) AS low_stock
  FROM order_line
  JOIN stock ON s_w_id = ol_w_id AND s_i_id = ol_i_id
 WHERE ol_w_id = :w_id
   AND ol_d_id = :d_id
   AND ol_o_id >= :min_o_id
   AND ol_o_id < :v_next_o_id
   AND s_quantity < :threshold \gset
\think_time random_exponential(1, 5, 2.0) ms
```

---

## 7. Cross-Dialect & Non-SQL Adaptation Quick Matrix (`Model B` & `Model C`)

| Target Engine / Language | Key Schema & Query Adaptations |
| :--- | :--- |
| **PostgreSQL / Oracle / SQL Server** | Use **`Model A` (Server-Side Stored Procedures, `Section 5`)** for maximum throughput and minimal lock contention on `warehouse`/`district`/`stock`. |
| **Cloud Spanner (GoogleSQL)** | Add `h_id STRING(36) DEFAULT (GENERATE_UUID())` PK to `history`; use `INTERLEAVE IN PARENT` (`warehouse` $\to$ `district` $\to$ `customer`, `orders` $\to$ `order_line`); use `THEN RETURN` instead of `RETURNING`; use `UNNEST(GENERATE_ARRAY(a, b, step))` instead of `generate_series()`. |
| **MySQL / MariaDB (InnoDB)** | Add `h_id BIGINT AUTO_INCREMENT PRIMARY KEY` to `history`; replace `UPDATE ... RETURNING` with `SELECT ... FOR UPDATE` + `UPDATE` (or wrap in MySQL `CREATE PROCEDURE`); use `CONCAT()` instead of `||`. |
| **DuckDB / Quack / SQLite** | Use pure client-driven DML (`Section 6`); `RETURNING` is natively supported. |
| **Neo4j Cypher (`--driver=neo4j`)** | Model entities as `:Warehouse`, `:District`, `:Customer`, `:Item`, `:Stock`, `:Order` (with `:NewOrder` label on pending orders; `REMOVE o:NewOrder` on `Delivery`), `:OrderLine`, `:History`. Use `OPTIONAL MATCH (it:Item {i_id: :i_1}) WITH w, d, c, it, (1 / CASE WHEN it IS NULL THEN 0 ELSE 1 END) AS item_ok` inside `\try ... \catch` for the 1% invalid-item rollback (see `doc/agent-guide-pgbench-order.md` Section 8.1). |
| **MongoDB MQL / Document DB** | Embed `order_lines[]` array and `pending_delivery: true` boolean inside `orders` documents; use `$inc` for `w_ytd`, `d_ytd`, `d_next_o_id`, `c_balance`, and `stock` counters inside multi-document ACID transactions. |
| **Key-Value / Redis / DynamoDB** | Use composite keys (`W:{w_id}`, `D:{w_id}:{d_id}`, `C:{w_id}:{d_id}:{c_id}`, `S:{w_id}:{i_id}`), sorted set `C_LAST:{w_id}:{d_id}:{c_last}` for midpoint customer lookup, and sorted set `NO_Q:{w_id}:{d_id}` (`ZADD` / `ZPOPMIN`) for the `new_order` FIFO delivery queue. |

---

## 8. Mandatory Post-Run Consistency Verification (`\validate_result`)

Every ported `pgbench-order` (`pgbench-c`) implementation must pass these **8 invariant assertions** after running a mixed workload (`pgbench -n -t 1 -f verify_pgbench_order_invariants.sql --sla="validation_failures<=0" "$DB"`):

```sql
-- Invariant 1: d_next_o_id - 1 == max(o_id) across all districts
SELECT count(*) AS inv1_violations
  FROM district d
  LEFT JOIN (
      SELECT o_w_id, o_d_id, max(o_id) AS max_o_id
        FROM orders GROUP BY o_w_id, o_d_id
  ) o ON o.o_w_id = d.d_w_id AND o.o_d_id = d.d_id
 WHERE d.d_next_o_id - 1 <> COALESCE(o.max_o_id, 0) \gset
\validate_result :inv1_violations 0

-- Invariant 2: new_order queue count == count(orders WHERE o_carrier_id IS NULL) per district
SELECT count(*) AS inv2_violations
  FROM district d
  LEFT JOIN (
      SELECT o_w_id, o_d_id, count(*) AS undeliv_cnt
        FROM orders WHERE o_carrier_id IS NULL GROUP BY o_w_id, o_d_id
  ) o ON o.o_w_id = d.d_w_id AND o.o_d_id = d.d_id
  LEFT JOIN (
      SELECT no_w_id, no_d_id, count(*) AS no_cnt
        FROM new_order GROUP BY no_w_id, no_d_id
  ) n ON n.no_w_id = d.d_w_id AND n.no_d_id = d.d_id
 WHERE COALESCE(o.undeliv_cnt, 0) <> COALESCE(n.no_cnt, 0) \gset
\validate_result :inv2_violations 0

-- Invariant 3: new_order queue contiguous range (max(no_o_id) - min(no_o_id) + 1 == count(new_order) and max(no_o_id) == max(o_id))
SELECT count(*) AS inv3_violations
  FROM district d
  LEFT JOIN (
      SELECT o_w_id, o_d_id, max(o_id) AS max_o_id
        FROM orders GROUP BY o_w_id, o_d_id
  ) o ON o.o_w_id = d.d_w_id AND o.o_d_id = d.d_id
  LEFT JOIN (
      SELECT no_w_id, no_d_id, count(*) AS no_cnt, min(no_o_id) AS min_no, max(no_o_id) AS max_no
        FROM new_order GROUP BY no_w_id, no_d_id
  ) n ON n.no_w_id = d.d_w_id AND n.no_d_id = d.d_id
 WHERE COALESCE(n.no_cnt, 0) > 0
   AND (n.max_no - n.min_no + 1 <> n.no_cnt OR n.max_no <> o.max_o_id) \gset
\validate_result :inv3_violations 0

-- Invariant 4: sum(o_ol_cnt) == count(order_line)
SELECT (SELECT COALESCE(sum(o_ol_cnt), 0) FROM orders) -
       (SELECT count(*) FROM order_line) AS inv4_diff \gset
\validate_result :inv4_diff 0

-- Invariant 5: o_carrier_id IS NULL iff ol_delivery_d IS NULL
SELECT count(*) AS inv5_violations
  FROM orders o
  JOIN order_line ol ON ol.ol_w_id = o.o_w_id AND ol.ol_d_id = o.o_d_id AND ol.ol_o_id = o.o_id
 WHERE (o.o_carrier_id IS NULL) <> (ol.ol_delivery_d IS NULL) \gset
\validate_result :inv5_violations 0

-- Invariant 6: w_ytd == sum(d_ytd) per warehouse
SELECT count(*) AS inv6_violations
  FROM warehouse w
  JOIN (
      SELECT d_w_id, sum(d_ytd) AS sum_d_ytd
        FROM district GROUP BY d_w_id
  ) d ON d.d_w_id = w.w_id
 WHERE w.w_ytd <> d.sum_d_ytd \gset
\validate_result :inv6_violations 0

-- Invariant 7: customer payment & history conservation + BC credit c_data audit trail
SELECT count(*) AS inv7_violations
  FROM customer c
  LEFT JOIN (
      SELECT h_c_w_id, h_c_d_id, h_c_id, count(*) AS h_cnt, sum(h_amount) AS h_sum
        FROM history GROUP BY h_c_w_id, h_c_d_id, h_c_id
  ) h ON h.h_c_w_id = c.c_w_id AND h.h_c_d_id = c.c_d_id AND h.h_c_id = c.c_id
 WHERE c.c_payment_cnt <> COALESCE(h.h_cnt, 0)
    OR c.c_ytd_payment <> COALESCE(h.h_sum, 0)
    OR (c.c_credit = 'BC' AND c.c_payment_cnt > 1 AND c.c_data NOT LIKE '% | %') \gset
\validate_result :inv7_violations 0

-- Invariant 8: customer balance & delivery count conservation against delivered order_line totals
SELECT count(*) AS inv8_violations
  FROM customer c
  LEFT JOIN (
      SELECT o.o_w_id, o.o_d_id, o.o_c_id,
             sum(ol.ol_amount) AS deliv_sum,
             count(DISTINCT CASE WHEN ol.ol_amount > 0 THEN o.o_id END) AS runtime_deliv_cnt
        FROM orders o
        JOIN order_line ol ON ol.ol_w_id = o.o_w_id AND ol.ol_d_id = o.o_d_id AND ol.ol_o_id = o.o_id
       WHERE o.o_carrier_id IS NOT NULL
       GROUP BY o.o_w_id, o.o_d_id, o.o_c_id
  ) d ON d.o_w_id = c.c_w_id AND d.o_d_id = c.c_d_id AND d.o_c_id = c.c_id
 WHERE c.c_balance + c.c_ytd_payment <> COALESCE(d.deliv_sum, 0)
    OR c.c_delivery_cnt <> COALESCE(d.runtime_deliv_cnt, 0) \gset
\validate_result :inv8_violations 0
```
