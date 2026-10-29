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
-- Social Graph Interactive Complex Reads (IC1-IC5, IC9, IC12, IC13)
-- Uses GoogleSQL GRAPH_TABLE and quantified path patterns over social_graph.
-- Discovers multi-hop friends & messages, pushes discovered IDs into shared
-- reservoirs via array-aware \reservoir_push, and chains into Short Reads via \call.
--

\if reservoir_count('persons') >= 0
\reservoir_sample persons person_id random_zipfian(1, 200 * :scale, 1.2)
\else
\set person_id random_zipfian(1, 200 * :scale, 1.2)
\endif

\set other_person_id community_correlated_edge(:person_id, 200 * :scale, 10, 0.50, 77)
\set cutoff_ts 1700090000000000
\set window_start_ts 1700000000000000

-- IC1 & IC2: 1..2 hop friends deduplicated via quantified path pattern -[k IS knows]->{1,2}
SELECT friend_id AS disc_person_id
  FROM (
      SELECT g.friend_id,
             g.last_name,
             MIN(g.dist) AS min_dist
        FROM GRAPH_TABLE (social_graph
            MATCH (src IS Person WHERE src.id = :person_id)-[k IS knows]->{1,2}(f IS Person)
            WHERE f.id <> :person_id
            RETURN f.id AS friend_id, f.last_name AS last_name, ARRAY_LENGTH(k) AS dist
        ) g
       GROUP BY g.friend_id, g.last_name
  )
 ORDER BY min_dist ASC, last_name ASC, friend_id ASC
 LIMIT 10 \gset_col disc_person_id

\if reservoir_count('persons') >= 0
\reservoir_push persons :disc_person_id
\endif

SELECT g.disc_msg_id
  FROM GRAPH_TABLE (social_graph
      MATCH (src IS Person WHERE src.id = :person_id)
            -[IS knows]->(f IS Person)
            <-[IS hasCreator]-(msg IS Message WHERE msg.created_ts <= :cutoff_ts)
      RETURN msg.id AS disc_msg_id, msg.created_ts AS created_ts
  ) g
 ORDER BY g.created_ts DESC
 LIMIT 10 \gset_col disc_msg_id

\if reservoir_count('messages') >= 0
\reservoir_push messages :disc_msg_id
\endif

-- IC3 & IC4: Popular tags among 1..2 hop friends' posts within time window
SELECT g.tag_name,
       count(*) AS post_count
  FROM (
      SELECT DISTINCT g.fid, g.msg_id, g.tag_id, g.tag_name
        FROM GRAPH_TABLE (social_graph
            MATCH (src IS Person WHERE src.id = :person_id)
                  -[k IS knows]->{1,2}(f IS Person WHERE f.id <> :person_id)
                  <-[IS hasCreator]-(m IS Message WHERE m.parent_message_id IS NULL AND m.created_ts >= :window_start_ts AND m.created_ts <= :cutoff_ts)
                  -[IS hasTag]->(t IS Tag)
            RETURN f.id AS fid, m.id AS msg_id, t.id AS tag_id, t.name AS tag_name
        ) g
  ) g
 GROUP BY g.tag_id, g.tag_name
 ORDER BY post_count DESC, g.tag_name ASC
 LIMIT 10;

-- IC5 & IC9: Forums joined and recent messages by friends-of-friends via GRAPH_TABLE
WITH fof AS (
    SELECT DISTINCT g.fid
      FROM GRAPH_TABLE (social_graph
          MATCH (src IS Person WHERE src.id = :person_id)
                -[IS knows]->(f1 IS Person)
                -[IS knows]->(f2 IS Person WHERE f2.id <> :person_id)
          RETURN f2.id AS fid
      ) g
),
fof_forums AS (
    SELECT DISTINCT fof.fid, g.forum_id, g.title
      FROM fof
      JOIN GRAPH_TABLE (social_graph
          MATCH (f IS Forum)-[IS hasMember]->(p IS Person)
          RETURN f.id AS forum_id, f.title AS title, p.id AS member_id
      ) g ON g.member_id = fof.fid
),
fof_msgs AS (
    SELECT DISTINCT ff.fid, ff.forum_id, g.msg_id
      FROM fof_forums ff
      JOIN GRAPH_TABLE (social_graph
          MATCH (f IS Forum)-[IS containerOf]->(m IS Message)-[IS hasCreator]->(p IS Person)
          RETURN f.id AS forum_id, m.id AS msg_id, p.id AS creator_id
      ) g ON g.forum_id = ff.forum_id AND g.creator_id = ff.fid
)
SELECT ff.forum_id,
       ff.title,
       count(fm.msg_id) AS msg_count
  FROM fof_forums ff
  LEFT JOIN fof_msgs fm
    ON fm.forum_id = ff.forum_id AND fm.fid = ff.fid
 GROUP BY ff.forum_id, ff.title
 ORDER BY msg_count DESC, ff.forum_id ASC
 LIMIT 10;

-- IC12: Expert friend replies on posts tagged in a given tagclass
SELECT g.friend_id,
       count(DISTINCT g.cmt_id) AS reply_count
  FROM GRAPH_TABLE (social_graph
      MATCH (src IS Person WHERE src.id = :person_id)
            -[IS knows]->(friend IS Person)
            <-[IS hasCreator]-(cmt IS Message WHERE cmt.parent_message_id IS NOT NULL)
            -[IS replyOf]->(post IS Message)
            -[IS hasTag]->(t IS Tag WHERE t.tagclass_id <= 5)
      RETURN friend.id AS friend_id, cmt.id AS cmt_id
  ) g
 GROUP BY g.friend_id
 ORDER BY reply_count DESC, g.friend_id ASC
 LIMIT 10;

-- IC13: Bounded Shortest Path between :person_id and :other_person_id via MATCH ANY SHORTEST
SELECT COALESCE(MIN(g.depth), -1) AS shortest_path_len
  FROM GRAPH_TABLE (social_graph
      MATCH ANY SHORTEST (src IS Person WHERE src.id = :person_id)-[k IS knows]->{1,4}(dst IS Person WHERE dst.id = :other_person_id)
      RETURN ARRAY_LENGTH(k) AS depth
  ) g \gset

\validate_result (:shortest_path_len >= -1) true

-- Chain into Social Graph Short Reads (IS1-IS7) via \call
\call pgbench_graph_pgq_spanner_social_short.sql person_id=:person_id
