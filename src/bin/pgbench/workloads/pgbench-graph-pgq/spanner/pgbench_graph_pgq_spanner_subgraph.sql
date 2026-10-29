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
-- Labelled Subgraph Pattern-Matching Suite (Q1 - Q9)
-- Executes unseeded cyclic (Q1-Q5), acyclic (Q6-Q7), and anti-join (Q8-Q9)
-- structural pattern-matching count queries over social_graph using GoogleSQL
-- GRAPH_TABLE syntax.
--

-- Q1: Long path across Country <-[:isPartOf]- City <-[:isLocatedIn]- Person <-[:hasMember]- Forum -[:containerOf]-> Post <-[:replyOf]- Comment -[:hasTag]-> Tag
WITH forum_members_loc AS (
    SELECT f_id
      FROM GRAPH_TABLE (social_graph
          MATCH (country IS Place WHERE country.type = 'country')
                <-[IS isPartOf]-(city IS Place WHERE city.type = 'city')
                <-[IS isLocatedIn]-(p IS Person)
                <-[IS hasMember]-(f IS Forum)
          RETURN f.id AS f_id
      )
),
post_replies AS (
    SELECT f_id, cmt_id
      FROM GRAPH_TABLE (social_graph
          MATCH (f IS Forum)
                -[IS containerOf]->(post IS Message WHERE post.parent_message_id IS NULL)
                <-[IS replyOf]-(cmt IS Message)
          RETURN f.id AS f_id, cmt.id AS cmt_id
      )
),
cmt_tags AS (
    SELECT cmt_id
      FROM GRAPH_TABLE (social_graph
          MATCH (cmt IS Message)-[IS hasTag]->(t IS Tag)
          RETURN cmt.id AS cmt_id
      )
)
SELECT count(*) AS q1_count
  FROM forum_members_loc fm
  JOIN @{join_type=HASH_JOIN} post_replies pr ON pr.f_id = fm.f_id
  JOIN @{join_type=HASH_JOIN} cmt_tags ct ON ct.cmt_id = pr.cmt_id \gset

\validate_result (:q1_count >= 0) true

-- Q2: Cyclic 4-pattern: Person1 -[:knows]-> Person2 <-[:hasCreator]- Comment -[:replyOf]-> Post -[:hasCreator]-> Person1
SELECT count(*) AS q2_count
  FROM GRAPH_TABLE (social_graph
      MATCH (p1 IS Person)-[IS knows]->(p2 IS Person)
            <-[IS hasCreator]-(cmt IS Message)
            -[IS replyOf]->(post IS Message WHERE post.parent_message_id IS NULL)
            -[IS hasCreator]->(p1 IS Person)
      RETURN 1 AS one
  ) \gset

\validate_result (:q2_count >= 0) true

-- Q3: Country-localized friendship triangle:
--     Person1 -[:knows]-> Person2 -[:knows]-> Person3 -[:knows]-> Person1
--     all located in cities of the same country (Chiba-Nishizeki degree-ordered HASH_JOIN)
WITH pc AS (
    SELECT p_id, country_id
      FROM GRAPH_TABLE (social_graph
          MATCH (p IS Person)-[IS isLocatedIn]->(c IS Place)-[IS isPartOf]->(country IS Place)
          RETURN p.id AS p_id, country.id AS country_id
      )
),
same_country_edges AS (
    SELECT k.src_id, k.dst_id
      FROM GRAPH_TABLE (social_graph
          MATCH (p1 IS Person)-[k IS knows]->(p2 IS Person)
          RETURN k.src_id AS src_id, k.dst_id AS dst_id
      ) k
      JOIN @{join_type=HASH_JOIN} pc pc1 ON pc1.p_id = k.src_id
      JOIN @{join_type=HASH_JOIN} pc pc2 ON pc2.p_id = k.dst_id AND pc2.country_id = pc1.country_id
),
deg AS (
    SELECT src_id AS vid, count(*) AS d
      FROM same_country_edges
     GROUP BY src_id
),
low_to_high AS (
    SELECT e.src_id AS u, e.dst_id AS v
      FROM same_country_edges e
      JOIN @{join_type=HASH_JOIN} deg du ON du.vid = e.src_id
      JOIN @{join_type=HASH_JOIN} deg dv ON dv.vid = e.dst_id
     WHERE du.d < dv.d OR (du.d = dv.d AND e.src_id < e.dst_id)
)
SELECT count(*) AS q3_count
  FROM low_to_high e1
  JOIN @{join_type=HASH_JOIN} low_to_high e2 ON e2.u = e1.u
  JOIN @{join_type=HASH_JOIN} low_to_high e3 ON e3.u = e1.v AND e3.v = e2.v \gset

\validate_result (:q3_count >= 0) true

-- Q4: Message - Tag - Like - Creator cyclic interaction pattern
SELECT count(*) AS q4_count
  FROM GRAPH_TABLE (social_graph
      MATCH (t IS Tag)<-[IS hasTag]-(m IS Message)<-[IS likes]-(liker IS Person)<-[IS knows]-(creator IS Person)<-[IS hasCreator]-(m IS Message)
      RETURN 1 AS one
  ) \gset

\validate_result (:q4_count >= 0) true

-- Q5: Message <-[:replyOf]- Comment chain with distinct tags (tag1 <> tag2)
SELECT count(*) AS q5_count
  FROM GRAPH_TABLE (social_graph
      MATCH (t1 IS Tag)<-[IS hasTag]-(m IS Message)<-[IS replyOf]-(cmt IS Message)-[IS hasTag]->(t2 IS Tag)
      WHERE t1.id <> t2.id
      RETURN 1 AS one
  ) \gset

\validate_result (:q5_count >= 0) true

-- Q6: 2-hop Acyclic Path: Person1 -[:knows]-> Person2 -[:knows]-> Person3 -[:hasInterest]-> Tag (Person1 <> Person3)
--     Evaluated via hub-pivoted GRAPH_TABLE degree aggregation around p2
WITH edges AS (
    SELECT src_id, dst_id
      FROM GRAPH_TABLE (social_graph
          MATCH (p2 IS Person)-[k IS knows]->(p3 IS Person)
          RETURN k.src_id AS src_id, k.dst_id AS dst_id
      )
),
p2_in_deg AS (
    SELECT dst_id AS p2_id, count(*) AS in_deg
      FROM edges
     GROUP BY dst_id
),
p3_tags AS (
    SELECT p_id, count(*) AS tag_cnt
      FROM GRAPH_TABLE (social_graph
          MATCH (p3 IS Person)-[IS hasInterest]->(t IS Tag)
          RETURN p3.id AS p_id
      )
     GROUP BY p_id
)
SELECT COALESCE(sum((d.in_deg - 1) * t.tag_cnt), 0) AS q6_count
  FROM edges e
  JOIN @{join_type=HASH_JOIN} p2_in_deg d ON d.p2_id = e.src_id
  JOIN @{join_type=HASH_JOIN} p3_tags t ON t.p_id = e.dst_id \gset

\validate_result (:q6_count >= 0) true

-- Q7: Star pattern with OPTIONAL (LEFT JOIN) likes and replies
SELECT count(*) AS q7_count
  FROM GRAPH_TABLE (social_graph
      MATCH (m IS Message)-[IS hasTag]->(t IS Tag)
      RETURN m.id AS msg_id
  ) base
  LEFT JOIN GRAPH_TABLE (social_graph
      MATCH (liker IS Person)-[IS likes]->(m IS Message)
      RETURN m.id AS msg_id, liker.id AS liker_id
  ) l ON l.msg_id = base.msg_id
  LEFT JOIN GRAPH_TABLE (social_graph
      MATCH (cmt IS Message)-[IS replyOf]->(m IS Message)
      RETURN m.id AS msg_id, cmt.id AS cmt_id
  ) r ON r.msg_id = base.msg_id \gset

\validate_result (:q7_count >= 0) true

-- Q8: Anti-Join Pattern: Message <-[:replyOf]- Comment where Comment has tag2 but NOT tag1 of parent Message
SELECT count(*) AS q8_count
  FROM GRAPH_TABLE (social_graph
      MATCH (t1 IS Tag)<-[IS hasTag]-(m IS Message)<-[IS replyOf]-(cmt IS Message)-[IS hasTag]->(t2 IS Tag)
      WHERE t1.id <> t2.id
      RETURN cmt.id AS cmt_id, t1.id AS tag1_id
  ) g
  LEFT JOIN GRAPH_TABLE (social_graph
      MATCH (c IS Message)-[IS hasTag]->(t IS Tag)
      RETURN c.id AS cmt_id, t.id AS tag1_id
  ) anti ON anti.cmt_id = g.cmt_id AND anti.tag1_id = g.tag1_id
 WHERE anti.cmt_id IS NULL \gset

\validate_result (:q8_count >= 0) true

-- Q9: Open-Triad Anti-Join: Person1 -[:knows]-> Person2 -[:knows]-> Person3 -[:hasInterest]-> Tag
--     where p3.id > p1.id AND NOT (Person1 -[:knows]-> Person3)
--     Evaluated via ordered wedge rank (UNNEST WITH OFFSET) minus Chiba-Nishizeki closed-triangle tag paths
WITH edges AS (
    SELECT src_id, dst_id
      FROM GRAPH_TABLE (social_graph
          MATCH (src IS Person)-[k IS knows]->(dst IS Person)
          RETURN k.src_id AS src_id, k.dst_id AS dst_id
      )
),
p3_tags AS (
    SELECT p_id, count(*) AS tag_cnt
      FROM GRAPH_TABLE (social_graph
          MATCH (p IS Person)-[IS hasInterest]->(t IS Tag)
          RETURN p.id AS p_id
      )
     GROUP BY p_id
),
adj AS (
    SELECT src_id,
           ARRAY_AGG(dst_id ORDER BY dst_id ASC) AS nbrs
      FROM edges
     GROUP BY src_id
),
ordered_wedges AS (
    SELECT p3_id, smaller_p1_cnt
      FROM adj,
      UNNEST(adj.nbrs) AS p3_id WITH OFFSET AS smaller_p1_cnt
),
total_wedge_paths AS (
    SELECT COALESCE(sum(ow.smaller_p1_cnt * COALESCE(pt.tag_cnt, 0)), 0) AS total_paths
      FROM ordered_wedges ow
      JOIN @{join_type=HASH_JOIN} p3_tags pt ON pt.p_id = ow.p3_id
),
deg AS (
    SELECT src_id AS vid, ARRAY_LENGTH(nbrs) AS d
      FROM adj
),
low_to_high AS (
    SELECT e.src_id AS u, e.dst_id AS v
      FROM edges e
      JOIN @{join_type=HASH_JOIN} deg du ON du.vid = e.src_id
      JOIN @{join_type=HASH_JOIN} deg dv ON dv.vid = e.dst_id
     WHERE du.d < dv.d OR (du.d = dv.d AND e.src_id < e.dst_id)
),
unique_tri AS (
    SELECT LEAST(e1.u, e1.v, e2.v) AS x1,
           (e1.u + e1.v + e2.v) - LEAST(e1.u, e1.v, e2.v) - GREATEST(e1.u, e1.v, e2.v) AS x2,
           GREATEST(e1.u, e1.v, e2.v) AS x3
      FROM low_to_high e1
      JOIN @{join_type=HASH_JOIN} low_to_high e2 ON e2.u = e1.u
      JOIN @{join_type=HASH_JOIN} low_to_high e3 ON e3.u = e1.v AND e3.v = e2.v
),
closed_tri_paths AS (
    SELECT COALESCE(sum(2 * COALESCE(pt3.tag_cnt, 0) + COALESCE(pt2.tag_cnt, 0)), 0) AS closed_paths
      FROM unique_tri ut
      LEFT JOIN @{join_type=HASH_JOIN} p3_tags pt3 ON pt3.p_id = ut.x3
      LEFT JOIN @{join_type=HASH_JOIN} p3_tags pt2 ON pt2.p_id = ut.x2
)
SELECT (tw.total_paths - ct.closed_paths) AS q9_count
  FROM total_wedge_paths tw
  CROSS JOIN closed_tri_paths ct \gset

\validate_result (:q9_count >= 0) true

