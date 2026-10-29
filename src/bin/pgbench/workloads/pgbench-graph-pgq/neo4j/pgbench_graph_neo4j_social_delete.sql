--
-- Copyright (c) 2026, PostgreSQL Global Development Group
-- 
-- pgbench-graph (Neo4j): Social Graph Interactive Deletes (DEL1 - DEL8)
--

\set del_type random(1, 4)
\set event_ts (1700200000000000 + :client_id * 1000 + random(1, 50000))
\set p1_id random(1, 200 * :scale)
\set p2_id community_correlated_edge(:p1_id, 200 * :scale, 10, 0.75, 99)

\begin_dependent_write :event_ts CHANNEL 0

\if :del_type = 1
-- DEL6 / DEL7: Cascading Comment Sub-Thread Deletion
OPTIONAL MATCH (root:Message)<-[:replyOf*0..3]-(m:Message)
WHERE root.parent_message_id IS NOT NULL AND root.id > 400 * :scale
DETACH DELETE m;

\elif :del_type = 2
-- DEL2 / DEL3: Remove a Like
OPTIONAL MATCH (p:Person {id: :p1_id})-[r:likes]->(m:Message)
WITH r LIMIT 1
DELETE r;

\elif :del_type = 3
-- DEL5: Remove Forum Membership
OPTIONAL MATCH (f:Forum)-[r:hasMember]->(p:Person {id: :p1_id})
WHERE f.id > 20 * :scale
WITH r LIMIT 1
DELETE r;

\else
-- DEL8: Remove Friendship Edge
OPTIONAL MATCH (p1:Person {id: :p1_id})-[r:knows]-(p2:Person {id: :p2_id})
WHERE r.created_ts >= 1700100000000000
DELETE r;

\endif

\end_dependent_write :event_ts CHANNEL 0
