/*
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * DISCLAIMER: This workload is an independent engineering benchmark designed
 * for database performance research and regression testing. While modeled
 * after graph traversal patterns published by the Graph Data Council (LDBC),
 * this is NOT an LDBC Benchmark, is NOT an implementation of an LDBC Benchmark,
 * and these results are NOT LDBC Benchmark Results.
 * LDBC, LDBC Benchmark, and LDBC Benchmark Results are registered trademarks
 * of the Graph Data Council. Distributed under CC-BY 4.0 attribution terms.
 */

--
-- pgbench-graph-pgq (Spanner Graph / GoogleSQL):
-- Social Graph Interactive Cascading Deletes (DEL1 - DEL8)
-- Executes recursive reply-subthread cascade deletions using GRAPH_TABLE
-- quantified path patterns, edge removals, and forum/member cleanup while
-- coordinating with the causal dependency watermark.
--

\set del_type random(1, 4)
\set event_ts (1700200000000000 + :client_id * 1000 + random(1, 50000))
\set p1_id random(1, 200 * :scale)
\set p2_id community_correlated_edge(:p1_id, 200 * :scale, 10, 0.75, 99)

\begin_dependent_write :event_ts CHANNEL 0

\if :del_type = 1
-- DEL6 / DEL7: Cascading Comment Sub-Thread Deletion via GRAPH_TABLE quantified replyOf path
BEGIN;

DELETE FROM pgbench_graph_likes
 WHERE message_id IN (
     SELECT g.id
       FROM GRAPH_TABLE (social_graph
           MATCH (root IS Message WHERE root.parent_message_id IS NOT NULL AND root.id > 400 * :scale)
                 <-[IS replyOf]-{0,3}(m IS Message)
           RETURN m.id AS id
       ) g
 );

DELETE FROM pgbench_graph_message_tags
 WHERE message_id IN (
     SELECT g.id
       FROM GRAPH_TABLE (social_graph
           MATCH (root IS Message WHERE root.parent_message_id IS NOT NULL AND root.id > 400 * :scale)
                 <-[IS replyOf]-{0,3}(m IS Message)
           RETURN m.id AS id
       ) g
 );

DELETE FROM pgbench_graph_messages
 WHERE id IN (
     SELECT g.id
       FROM GRAPH_TABLE (social_graph
           MATCH (root IS Message WHERE root.parent_message_id IS NOT NULL AND root.id > 400 * :scale)
                 <-[IS replyOf]-{0,3}(m IS Message)
           RETURN m.id AS id
       ) g
 );

COMMIT;

\elif :del_type = 2
-- DEL2 / DEL3: Remove a Post/Comment Like discovered via GRAPH_TABLE
DELETE FROM pgbench_graph_likes
 WHERE STRUCT(person_id, message_id) IN (
     SELECT AS STRUCT g.person_id, g.message_id
       FROM GRAPH_TABLE (social_graph
           MATCH (p IS Person WHERE p.id = :p1_id)-[IS likes]->(m IS Message)
           RETURN p.id AS person_id, m.id AS message_id
       ) g
      LIMIT 1
 );

\elif :del_type = 3
-- DEL5: Remove Forum Membership discovered via GRAPH_TABLE
DELETE FROM pgbench_graph_forum_members
 WHERE STRUCT(forum_id, person_id) IN (
     SELECT AS STRUCT g.forum_id, g.person_id
       FROM GRAPH_TABLE (social_graph
           MATCH (f IS Forum WHERE f.id > 20 * :scale)-[IS hasMember]->(p IS Person WHERE p.id = :p1_id)
           RETURN f.id AS forum_id, p.id AS person_id
       ) g
      LIMIT 1
 );

\else
-- DEL8: Remove Bidirectional Friendship Edge (for dynamically added edges)
DELETE FROM pgbench_graph_knows
 WHERE (src_id = :p1_id AND dst_id = :p2_id AND created_ts >= 1700100000000000)
    OR (src_id = :p2_id AND dst_id = :p1_id AND created_ts >= 1700100000000000);
\endif

\end_dependent_write :event_ts CHANNEL 0
