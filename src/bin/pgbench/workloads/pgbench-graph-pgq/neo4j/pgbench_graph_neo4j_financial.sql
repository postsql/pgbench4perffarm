--
-- Copyright (c) 2026, PostgreSQL Global Development Group
-- 
-- pgbench-graph (Neo4j): Financial Graph Workload Suite (TSR, TCR, TW, TRW)
--

\set acct_id random_zipfian(1, 200 * :scale, 1.2)
\set dst_acct community_correlated_edge(:acct_id, 200 * :scale, 8, 0.70, 101)
\set start_ts 1700000000000000
\set end_ts   1700500000000000
\set event_ts (1700300000000000 + :client_id * 1000 + random(1, 50000))

-- TSR1: Account status and balance
MATCH (a:Account {id: :acct_id})
RETURN a.id AS id, a.owner_id AS owner_id, a.owner_type AS owner_type, a.is_blocked AS is_blocked, a.balance AS balance;

-- TSR2 & TSR3: Outgoing transfer aggregations
MATCH (src:Account {id: :acct_id})-[t:transfer]->(dst:Account)
WHERE t.created_ts >= :start_ts AND t.created_ts <= :end_ts
RETURN count(t) AS out_cnt,
       coalesce(sum(t.amount), 0.0) AS out_sum,
       coalesce(max(t.amount), 0.0) AS out_max,
       count(CASE WHEN dst.is_blocked THEN 1 END) AS blocked_dst_cnt \gset

\validate_result (:out_cnt >= 0) true

-- TSR6: Accounts sharing a sign-in medium
MATCH (a1:Account {id: :acct_id})<-[:signIn]-(m:Medium)-[:signIn]->(a2:Account)
WHERE a2.id <> a1.id
RETURN DISTINCT a2.id AS shared_acct_id, m.id AS medium_id, m.risk_score AS risk_score
ORDER BY risk_score DESC, shared_acct_id ASC
LIMIT 10;

-- TCR2 & TCR8: Loan-to-Transfer Outflow Ratio Tracking
MATCH (l:Loan {borrower_id: :acct_id})-[:deposit]->(a:Account)
OPTIONAL MATCH (a)-[t:transfer]->(dst:Account)
WHERE t.created_ts >= l.created_ts
RETURN coalesce(sum(l.loan_amount), 0.0) AS total_loan_amt,
       coalesce(sum(t.amount), 0.0) AS downstream_transfer_amt \gset

\validate_result (:total_loan_amt >= 0) true

-- 3. Write Operation (TW): Insert a new sign-in event
\begin_dependent_write :event_ts CHANNEL 1
MATCH (m:Medium {id: 1 + (:acct_id % (50 * :scale))}), (a:Account {id: :acct_id})
MERGE (m)-[r:signIn {created_ts: :event_ts}]->(a);
\end_dependent_write :event_ts CHANNEL 1

-- 4. Interactive Read-Write Fraud Prevention Transactions (TRW1 - TRW3)
\set trw_mode random(1, 2)
\if :trw_mode = 1
\set trw_dst 17
\else
\set trw_dst :dst_acct
\endif

BEGIN;

MATCH (src:Account {id: :acct_id}), (dst:Account {id: :trw_dst})
CREATE (src)-[r:transfer {id: timestamp(), amount: 950.0, created_ts: :event_ts + 1}]->(dst);

MATCH (a:Account)
WHERE a.id IN [:acct_id, :trw_dst] AND a.is_blocked
RETURN (count(a) > 0) AS fraud_detected \gset

\if :fraud_detected
ROLLBACK;
\else
COMMIT;
\endif
