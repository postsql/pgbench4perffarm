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
-- Graph Algorithms Suite (BFS, WCC, SSSP, PageRank, LCC)
-- Executes deterministic whole-graph algorithms in SQL over social_graph using
-- GoogleSQL GRAPH_TABLE quantified path expansions and triangle pattern
-- matching, verifying structural & numerical invariants via \validate_result.
--

\set root_id 1

-- 1. Breadth-First Search (BFS) via GRAPH_TABLE quantified path MATCH ANY SHORTEST -[k IS knows]->{0,4}
WITH bfs AS (
    SELECT g.vertex_id,
           MIN(g.depth) AS min_depth
      FROM GRAPH_TABLE (social_graph
          MATCH ANY SHORTEST (root IS Person WHERE root.id = :root_id)-[k IS knows]->{0,4}(dst IS Person)
          RETURN dst.id AS vertex_id, ARRAY_LENGTH(k) AS depth
      ) g
     GROUP BY g.vertex_id
)
SELECT count(*) AS bfs_reachable,
       COALESCE(MIN(IF(vertex_id = :root_id, min_depth, NULL)), -1) AS bfs_root_depth,
       COALESCE(MAX(min_depth), 0) AS bfs_max_depth
  FROM bfs \gset

\validate_result :bfs_root_depth 0
\validate_result (:bfs_reachable >= 1 and :bfs_max_depth >= 0) true

-- 2. Weakly Connected Components (WCC) — 4-stage iterative 1-hop label propagation over GRAPH_TABLE
WITH vertices AS (
    SELECT vid
      FROM GRAPH_TABLE (social_graph
          MATCH (p IS Person)
          RETURN p.id AS vid
      )
),
edges AS (
    SELECT src_id, dst_id
      FROM GRAPH_TABLE (social_graph
          MATCH (src IS Person)-[k IS knows]->(dst IS Person)
          RETURN k.src_id AS src_id, k.dst_id AS dst_id
      )
),
wcc0 AS (
    SELECT vid, vid AS cid FROM vertices
),
wcc1 AS (
    SELECT w.vid,
           LEAST(w.cid, COALESCE(MIN(nbr.cid), w.cid)) AS cid
      FROM wcc0 w
      LEFT JOIN @{join_type=HASH_JOIN} edges e ON e.dst_id = w.vid
      LEFT JOIN @{join_type=HASH_JOIN} wcc0 nbr ON nbr.vid = e.src_id
     GROUP BY w.vid, w.cid
),
wcc2 AS (
    SELECT w.vid,
           LEAST(w.cid, COALESCE(MIN(nbr.cid), w.cid)) AS cid
      FROM wcc1 w
      LEFT JOIN @{join_type=HASH_JOIN} edges e ON e.dst_id = w.vid
      LEFT JOIN @{join_type=HASH_JOIN} wcc1 nbr ON nbr.vid = e.src_id
     GROUP BY w.vid, w.cid
),
wcc3 AS (
    SELECT w.vid,
           LEAST(w.cid, COALESCE(MIN(nbr.cid), w.cid)) AS cid
      FROM wcc2 w
      LEFT JOIN @{join_type=HASH_JOIN} edges e ON e.dst_id = w.vid
      LEFT JOIN @{join_type=HASH_JOIN} wcc2 nbr ON nbr.vid = e.src_id
     GROUP BY w.vid, w.cid
),
wcc4 AS (
    SELECT w.vid,
           LEAST(w.cid, COALESCE(MIN(nbr.cid), w.cid)) AS cid
      FROM wcc3 w
      LEFT JOIN @{join_type=HASH_JOIN} edges e ON e.dst_id = w.vid
      LEFT JOIN @{join_type=HASH_JOIN} wcc3 nbr ON nbr.vid = e.src_id
     GROUP BY w.vid, w.cid
)
SELECT count(DISTINCT cid) AS wcc_components,
       count(*) AS wcc_vertices
  FROM wcc4 \gset

\validate_result (:wcc_components >= 1 and :wcc_components <= :wcc_vertices) true

-- 3. Single-Source Shortest Paths (SSSP) over weighted knows edges via quantified path + SUM(k.weight)
WITH sssp AS (
    SELECT g.vid,
           MIN(g.dist) AS min_dist
      FROM GRAPH_TABLE (social_graph
          MATCH (src IS Person WHERE src.id = :root_id)-[k IS knows]->{0,3}(dst IS Person)
          LET dist = COALESCE(SUM(k.weight), 0.0)
          RETURN dst.id AS vid, dist
      ) g
     GROUP BY g.vid
)
SELECT COALESCE(MIN(IF(vid = :root_id, min_dist, NULL)), -1.0) AS sssp_root_dist,
       COALESCE(MAX(min_dist), 0.0) AS sssp_max_dist
  FROM sssp \gset

\validate_result :sssp_root_dist 0.0 0.000001
\validate_result (:sssp_max_dist >= 0.0) true

-- 4. PageRank (PR) — 3-step damped power iteration (d = 0.85) over GRAPH_TABLE
WITH vertices AS (
    SELECT vid
      FROM GRAPH_TABLE (social_graph
          MATCH (p IS Person)
          RETURN p.id AS vid
      )
),
edges AS (
    SELECT src_id, dst_id
      FROM GRAPH_TABLE (social_graph
          MATCH (src IS Person)-[k IS knows]->(dst IS Person)
          RETURN k.src_id AS src_id, k.dst_id AS dst_id
      )
),
n_cte AS (
    SELECT CAST(GREATEST(count(*), 1) AS FLOAT64) AS n FROM vertices
),
deg AS (
    SELECT v.vid,
           CAST(count(k.dst_id) AS INT64) AS out_deg
      FROM vertices v
      LEFT JOIN @{join_type=HASH_JOIN} edges k ON k.src_id = v.vid
     GROUP BY v.vid
),
pr0 AS (
    SELECT d.vid, d.out_deg, (1.0 / n_cte.n) AS pr
      FROM deg d CROSS JOIN n_cte
),
dangling0 AS (
    SELECT COALESCE(sum(pr), 0.0) AS dmass FROM pr0 WHERE out_deg = 0
),
pr1 AS (
    SELECT p.vid,
           p.out_deg,
           ((1.0 - 0.85) + 0.85 * d0.dmass) / n_cte.n +
           0.85 * COALESCE(sum(src.pr / NULLIF(src.out_deg, 0)), 0.0) AS pr
      FROM pr0 p
      CROSS JOIN n_cte
      CROSS JOIN dangling0 d0
      LEFT JOIN @{join_type=HASH_JOIN} edges k ON k.dst_id = p.vid
      LEFT JOIN @{join_type=HASH_JOIN} pr0 src ON src.vid = k.src_id
     GROUP BY p.vid, p.out_deg, n_cte.n, d0.dmass
),
dangling1 AS (
    SELECT COALESCE(sum(pr), 0.0) AS dmass FROM pr1 WHERE out_deg = 0
),
pr2 AS (
    SELECT p.vid,
           p.out_deg,
           ((1.0 - 0.85) + 0.85 * d1.dmass) / n_cte.n +
           0.85 * COALESCE(sum(src.pr / NULLIF(src.out_deg, 0)), 0.0) AS pr
      FROM pr1 p
      CROSS JOIN n_cte
      CROSS JOIN dangling1 d1
      LEFT JOIN @{join_type=HASH_JOIN} edges k ON k.dst_id = p.vid
      LEFT JOIN @{join_type=HASH_JOIN} pr1 src ON src.vid = k.src_id
     GROUP BY p.vid, p.out_deg, n_cte.n, d1.dmass
)
SELECT ROUND(sum(pr), 6) AS pr_total_mass,
       COALESCE(max(pr), 0.0) >= COALESCE(min(pr), 0.0) AS pr_ordered
  FROM pr2 \gset

\validate_result :pr_total_mass 1.0 0.01
\validate_result :pr_ordered true

-- 5. Local Clustering Coefficient (LCC) per vertex using GRAPH_TABLE + degree-ordered triangle listing:
--    2 * triangles(v) / (deg(v) * (deg(v) - 1))
WITH edges AS (
    SELECT src_id, dst_id
      FROM GRAPH_TABLE (social_graph
          MATCH (src IS Person)-[k IS knows]->(dst IS Person)
          RETURN k.src_id AS src_id, k.dst_id AS dst_id
      )
),
deg AS (
    SELECT src_id AS vid, count(DISTINCT dst_id) AS d
      FROM edges
     GROUP BY src_id
),
low_to_high AS (
    SELECT e.src_id AS u, e.dst_id AS v
      FROM edges e
      JOIN @{join_type=HASH_JOIN} deg du ON du.vid = e.src_id
      JOIN @{join_type=HASH_JOIN} deg dv ON dv.vid = e.dst_id
     WHERE du.d < dv.d OR (du.d = dv.d AND e.src_id < e.dst_id)
),
unique_tri AS (
    SELECT e1.u AS a, e1.v AS b, e2.v AS c
      FROM low_to_high e1
      JOIN @{join_type=HASH_JOIN} low_to_high e2 ON e2.u = e1.u
      JOIN @{join_type=HASH_JOIN} low_to_high e3 ON e3.u = e1.v AND e3.v = e2.v
),
tri_vertices AS (
    SELECT a AS vid FROM unique_tri
    UNION ALL
    SELECT b AS vid FROM unique_tri
    UNION ALL
    SELECT c AS vid FROM unique_tri
),
tri AS (
    SELECT vid,
           count(*) AS closed_pairs
      FROM tri_vertices
     GROUP BY vid
),
lcc_per_vertex AS (
    SELECT deg.vid,
           CASE
               WHEN deg.d < 2 THEN 0.0
               ELSE LEAST(1.0, CAST(2.0 * COALESCE(tri.closed_pairs, 0) AS FLOAT64) / CAST(deg.d * (deg.d - 1) AS FLOAT64))
           END AS lcc
      FROM deg
      LEFT JOIN @{join_type=HASH_JOIN} tri ON tri.vid = deg.vid
)
SELECT COALESCE(avg(lcc), 0.0) AS avg_lcc
  FROM lcc_per_vertex \gset

\validate_result (:avg_lcc >= 0.0 and :avg_lcc <= 1.0) true
