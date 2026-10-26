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
-- Social Graph Business Intelligence (BI) Phased Workload Suite
-- Uses GoogleSQL GRAPH_TABLE pattern matching and quantified path patterns over social_graph.
-- Orchestrates sequential phases across worker clients using \phase_barrier:
--   Phase 1 (Power Run): BI-1, BI-2, BI-4, BI-9, BI-11, BI-19 analytical queries
--   \phase_barrier bi_microbatch
--   Phase 2 (Micro-Batch Update): Partitioned inserts + cascading sub-thread delete
--   \phase_barrier bi_throughput
--   Phase 3 (Throughput Run): Concurrent multi-stream BI analytical queries
--

\set cutoff_ts 1700090000000000
\set window1_start 1700000000000000
\set window1_end   1700040000000000
\set window2_end   1700080000000000
\set country_id (7 + (:client_id % 10))
\set city1_id (17 + (:client_id % 25))
\set city2_id (42 + (:client_id % 25))

-- ============================================================================
-- PHASE 1: BI Power Run (Analytical Queries BI-1, BI-2, BI-4, BI-9, BI-11, BI-19)
-- ============================================================================

-- BI-1: Posting Summary Report by message type (Post vs Comment) and length category
WITH msg_base AS (
    SELECT (g.parent_message_id IS NOT NULL) AS is_comment,
           CASE
               WHEN g.length < 40 THEN 0
               WHEN g.length < 80 THEN 1
               WHEN g.length < 160 THEN 2
               ELSE 3
           END AS length_cat,
           g.length
      FROM GRAPH_TABLE (social_graph
          MATCH (m IS Message WHERE (m.id <= 4000 OR m.id > 1000000000) AND m.created_ts <= :cutoff_ts)
          RETURN m.parent_message_id AS parent_message_id, m.length AS length
      ) g
),
totals AS (
    SELECT GREATEST(count(*), 1) AS total_msgs FROM msg_base
)
SELECT count(*) AS bi1_groups,
       COALESCE(sum(mb_cnt), 0) AS bi1_total_msgs
  FROM (
      SELECT mb.is_comment,
             mb.length_cat,
             count(*) AS mb_cnt,
             ROUND(CAST(avg(mb.length) AS NUMERIC), 2) AS avg_len,
             sum(mb.length) AS sum_len,
             ROUND(CAST(count(*) AS NUMERIC) / max(t.total_msgs), 4) AS pct_msgs
        FROM msg_base mb
        CROSS JOIN totals t
       GROUP BY mb.is_comment, mb.length_cat
  ) q \gset

\validate_result (:bi1_groups >= 1 and :bi1_total_msgs >= 1) true

-- BI-2: Tag Evolution across two contiguous time windows via GRAPH_TABLE
SELECT count(*) AS bi2_rows
  FROM (
      SELECT gt.tag_name,
             COUNTIF(gm.created_ts >= :window1_start AND gm.created_ts < :window1_end) AS cnt_w1,
             COUNTIF(gm.created_ts >= :window1_end AND gm.created_ts < :window2_end) AS cnt_w2,
             abs(
                 COUNTIF(gm.created_ts >= :window1_start AND gm.created_ts < :window1_end) -
                 COUNTIF(gm.created_ts >= :window1_end AND gm.created_ts < :window2_end)
             ) AS diff
        FROM GRAPH_TABLE (social_graph
            MATCH (t IS Tag WHERE t.tagclass_id <= 5)
            RETURN t.id AS tag_id, t.name AS tag_name
        ) gt
        LEFT JOIN GRAPH_TABLE (social_graph
            MATCH (m IS Message WHERE m.id <= 4000)-[IS hasTag]->(t IS Tag)
            RETURN t.id AS tag_id, m.created_ts AS created_ts
        ) gm ON gm.tag_id = gt.tag_id
       GROUP BY gt.tag_id, gt.tag_name
       ORDER BY diff DESC, gt.tag_name ASC
       LIMIT 20
  ) q \gset

\validate_result (:bi2_rows >= 1) true

-- BI-4: Top Message Creators in Country Forums via GRAPH_TABLE
WITH top_forums AS (
    SELECT g.forum_id
      FROM GRAPH_TABLE (social_graph
          MATCH (f IS Forum WHERE f.id <= 200)-[IS hasModerator]->(mod_p IS Person WHERE mod_p.id <= 1000)-[IS isLocatedIn]->(city IS Place WHERE city.part_of_id = :country_id)
          RETURN f.id AS forum_id
      ) g
     ORDER BY g.forum_id ASC
     LIMIT 10
)
SELECT count(*) AS bi4_rows
  FROM (
      SELECT gm.person_id,
             gm.first_name,
             gm.last_name,
             count(gmsg.msg_id) AS message_count
        FROM top_forums tf
        JOIN GRAPH_TABLE (social_graph
            MATCH (f IS Forum WHERE f.id <= 200)-[IS hasMember]->(p IS Person WHERE p.id <= 1000)
            RETURN f.id AS forum_id, p.id AS person_id, p.first_name AS first_name, p.last_name AS last_name
        ) gm ON gm.forum_id = tf.forum_id
        LEFT JOIN GRAPH_TABLE (social_graph
            MATCH (f IS Forum WHERE f.id <= 200)-[IS containerOf]->(m IS Message WHERE m.id <= 4000)-[IS hasCreator]->(p IS Person WHERE p.id <= 1000)
            RETURN f.id AS forum_id, m.id AS msg_id, p.id AS creator_id
        ) gmsg ON gmsg.forum_id = gm.forum_id AND gmsg.creator_id = gm.person_id
       GROUP BY gm.person_id, gm.first_name, gm.last_name
       ORDER BY message_count DESC, gm.person_id ASC
       LIMIT 20
  ) q \gset

\validate_result (:bi4_rows >= 0) true

-- BI-9: Top Thread Initiators via GRAPH_TABLE quantified reply-tree expansion
WITH thread_tree AS (
    SELECT DISTINCT
           g.root_post_id,
           g.root_author_id,
           g.msg_id
      FROM GRAPH_TABLE (social_graph
          MATCH (p IS Person WHERE p.id <= 1000)<-[IS hasCreator]-(post IS Message WHERE post.id <= 1000 AND post.parent_message_id IS NULL AND post.created_ts >= :window1_start AND post.created_ts <= :cutoff_ts)
                <-[r IS replyOf WHERE r.created_ts >= :window1_start AND r.created_ts <= :cutoff_ts]-{0,3}(m IS Message WHERE m.id <= 1000)
          RETURN post.id AS root_post_id, p.id AS root_author_id, m.id AS msg_id
      ) g
)
SELECT count(*) AS bi9_rows
  FROM (
      SELECT tt.root_author_id AS person_id,
             count(DISTINCT tt.root_post_id) AS thread_count,
             count(tt.msg_id) AS message_count
        FROM thread_tree tt
       GROUP BY tt.root_author_id
       ORDER BY message_count DESC, tt.root_author_id ASC
       LIMIT 15
  ) q \gset

\validate_result (:bi9_rows >= 1) true

-- BI-11: Friend Triangles within a Country via GRAPH_TABLE
SELECT count(*) AS bi11_triangles
  FROM GRAPH_TABLE (social_graph
      MATCH (p1 IS Person WHERE p1.id <= 200)-[IS knows]->(p2 IS Person WHERE p2.id <= 1000)-[IS knows]->(p3 IS Person WHERE p3.id <= 1000)-[IS knows]->(p1 IS Person)
      WHERE p1.id < p2.id AND p2.id < p3.id
      RETURN p1.city_id AS c1_id, p2.city_id AS c2_id, p3.city_id AS c3_id
  ) tri
  JOIN GRAPH_TABLE (social_graph
      MATCH (c1 IS Place)-[IS isPartOf]->(country IS Place)
      RETURN c1.id AS city_id, country.id AS country_id
  ) l1 ON l1.city_id = tri.c1_id
  JOIN GRAPH_TABLE (social_graph
      MATCH (c2 IS Place)-[IS isPartOf]->(country IS Place)
      RETURN c2.id AS city_id, country.id AS country_id
  ) l2 ON l2.city_id = tri.c2_id AND l2.country_id = l1.country_id
  JOIN GRAPH_TABLE (social_graph
      MATCH (c3 IS Place)-[IS isPartOf]->(country IS Place)
      RETURN c3.id AS city_id, country.id AS country_id
  ) l3 ON l3.city_id = tri.c3_id AND l3.country_id = l1.country_id \gset

\validate_result (:bi11_triangles >= 0) true

-- BI-19: Interaction-Weighted Shortest Path Between Two Cities via quantified path + SUM(k.weight)
SELECT COALESCE(min(g.total_weight), 0.0) AS bi19_min_weight
  FROM GRAPH_TABLE (social_graph
      MATCH (src IS Person WHERE src.id <= 1000 AND src.city_id = :city1_id)-[k IS knows WHERE k.src_id <= 1000 AND k.dst_id <= 1000]->{1,3}(dst IS Person WHERE dst.id <= 1000 AND dst.city_id = :city2_id)
      LET total_weight = SUM(k.weight)
      RETURN total_weight
  ) g \gset

\validate_result (:bi19_min_weight >= 0.0) true

-- ============================================================================
-- BARRIER 1: Synchronize all BI clients before Micro-Batch Refresh
-- ============================================================================
\phase_barrier bi_microbatch

-- ============================================================================
-- PHASE 2: BI Micro-Batch Update (Insert Batch + Cascading Reply Delete Batch)
-- ============================================================================
\set batch_ts (1700050000000000 + :client_id * 1000)
\set batch_creator (1 + (:client_id % (200 * :scale)))
\set batch_friend community_correlated_edge(:batch_creator, 200 * :scale, 10, 0.75, 55)

INSERT INTO pgbench_graph_messages (creator_id, forum_id, parent_message_id, country_id, content, length, created_ts)
VALUES (:batch_creator, 1 + MOD(:batch_creator, 20 * :scale), NULL, :country_id, 'BI Microbatch Post', 18, :batch_ts)
THEN RETURN id AS bi_post_id \gset

INSERT INTO pgbench_graph_messages (creator_id, forum_id, parent_message_id, country_id, content, length, created_ts)
VALUES (:batch_friend, 1 + MOD(:batch_creator, 20 * :scale), :bi_post_id, :country_id, 'BI Microbatch Reply', 19, :batch_ts + 100)
THEN RETURN id AS bi_reply_id \gset

INSERT OR IGNORE INTO pgbench_graph_message_tags (message_id, tag_id)
VALUES (:bi_post_id, 1 + MOD(:client_id, 100)),
       (:bi_reply_id, 2 + MOD(:client_id, 99));

-- Delete temporary reply sub-thread in micro-batch cleanup using GRAPH_TABLE quantified replyOf path
BEGIN;

DELETE FROM pgbench_graph_message_tags
 WHERE message_id IN (
     SELECT g.id
       FROM GRAPH_TABLE (social_graph
           MATCH (root IS Message WHERE root.id = :bi_reply_id)<-[IS replyOf]-{0,3}(m IS Message)
           RETURN m.id AS id
       ) g
 );

DELETE FROM pgbench_graph_messages
 WHERE id IN (
     SELECT g.id
       FROM GRAPH_TABLE (social_graph
           MATCH (root IS Message WHERE root.id = :bi_reply_id)<-[IS replyOf]-{0,3}(m IS Message)
           RETURN m.id AS id
       ) g
 );

COMMIT;

-- ============================================================================
-- BARRIER 2: Synchronize all BI clients before Multi-Stream Throughput Run
-- ============================================================================
\phase_barrier bi_throughput

-- ============================================================================
-- PHASE 3: BI Throughput Run (Concurrent Post-Refresh Analytical Verification)
-- ============================================================================
SELECT count(*) AS post_refresh_msgs
  FROM GRAPH_TABLE (social_graph
      MATCH (m IS Message WHERE (m.id <= 4000 OR m.id > 1000000000) AND m.created_ts <= :cutoff_ts)
      RETURN 1 AS one
  ) \gset

\validate_result (:post_refresh_msgs > :bi1_total_msgs) true
