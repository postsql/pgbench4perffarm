--
-- Copyright (c) 2026, PostgreSQL Global Development Group
-- 
-- pgbench-graph (Neo4j): Social Graph Business Intelligence (BI) Phased Suite
--

\set cutoff_ts 1700090000000000
\set window1_start 1700000000000000
\set window1_end   1700040000000000
\set window2_end   1700080000000000
\set country_id (7 + (:client_id % 10))
\set city1_id (17 + (:client_id % 25))
\set city2_id (42 + (:client_id % 25))

-- ============================================================================
-- PHASE 1: BI Power Run (BI-1, BI-2, BI-4, BI-9, BI-11, BI-19)
-- ============================================================================

-- BI-1: Posting Summary Report
MATCH (m:Message)
WHERE m.created_ts <= :cutoff_ts
WITH (m.parent_message_id IS NOT NULL) AS is_comment,
     CASE WHEN m.length < 40 THEN 0 WHEN m.length < 80 THEN 1 WHEN m.length < 160 THEN 2 ELSE 3 END AS length_cat,
     m.length AS length
RETURN is_comment, length_cat, count(*) AS mb_cnt, avg(length) AS avg_len, sum(length) AS sum_len;

-- BI-2: Tag Evolution across two contiguous time windows
MATCH (t:Tag) WHERE t.tagclass_id <= 5
OPTIONAL MATCH (m:Message)-[:hasTag]->(t)
WITH t.name AS tag_name,
     count(CASE WHEN m.created_ts >= :window1_start AND m.created_ts < :window1_end THEN 1 END) AS cnt_w1,
     count(CASE WHEN m.created_ts >= :window1_end AND m.created_ts < :window2_end THEN 1 END) AS cnt_w2
RETURN tag_name, cnt_w1, cnt_w2, abs(cnt_w1 - cnt_w2) AS diff
ORDER BY diff DESC, tag_name ASC
LIMIT 20;

-- BI-4: Top Message Creators in Country Forums
MATCH (country:Place {id: :country_id})<-[:isPartOf]-(:Place)<-[:isLocatedIn]-(:Person)<-[:hasModerator]-(f:Forum)-[:containerOf]->(m:Message)-[:hasCreator]->(p:Person)
RETURN p.id AS person_id, p.first_name AS first_name, p.last_name AS last_name, count(m) AS msg_count
ORDER BY msg_count DESC, person_id ASC
LIMIT 20;

-- BI-9: Top Creators by Message & Reply Volume in Country
MATCH (p:Person)-[:isLocatedIn]->(:Place)-[:isPartOf]->(country:Place {id: :country_id})
OPTIONAL MATCH (m:Message)-[:hasCreator]->(p)
WHERE m.created_ts >= :window1_start AND m.created_ts < :window2_end
OPTIONAL MATCH (cmt:Message)-[:replyOf]->(m)
RETURN p.id AS person_id, count(DISTINCT m) AS msg_count, count(cmt) AS reply_count
ORDER BY msg_count DESC, reply_count DESC, person_id ASC
LIMIT 20;

-- BI-11: Friend Pairs Between Two Cities
MATCH (p1:Person)-[:knows]->(p2:Person)
MATCH (p1)-[:isLocatedIn]->(c1:Place {id: :city1_id})
MATCH (p2)-[:isLocatedIn]->(c2:Place {id: :city2_id})
RETURN count(*) AS friend_pair_count;

-- BI-19: Interaction Path Connectivity Between Cities
MATCH (p1:Person)-[:isLocatedIn]->(c1:Place {id: :city1_id})
WITH p1 LIMIT 10
MATCH (p2:Person)-[:isLocatedIn]->(c2:Place {id: :city2_id})
WITH p1, p2 LIMIT 100
MATCH path = shortestPath((p1)-[:knows*..3]-(p2))
RETURN count(path) AS connected_pairs \gset

\validate_result (:connected_pairs >= 0) true

\phase_barrier bi_microbatch

-- ============================================================================
-- PHASE 2: Micro-Batch Update
-- ============================================================================

\set batch_base (1700400000000000 + :client_id * 10000000 + random(1, 9999999))

MATCH (pl:Place {id: :city1_id})
CREATE (p:Person {
    id: :batch_base,
    first_name: 'BIPerson',
    last_name: 'Stream',
    gender: 'female',
    city_id: :city1_id,
    company_id: 1,
    university_id: 1,
    created_ts: :batch_base
})-[:isLocatedIn]->(pl);

\phase_barrier bi_throughput

-- ============================================================================
-- PHASE 3: Throughput Run
-- ============================================================================

MATCH (country:Place {id: :country_id})<-[:isPartOf]-(:Place)<-[:isLocatedIn]-(:Person)<-[:hasModerator]-(f:Forum)-[:containerOf]->(m:Message)-[:hasCreator]->(p:Person)
RETURN p.id AS person_id, count(m) AS msg_count
ORDER BY msg_count DESC, person_id ASC
LIMIT 10;
