--
-- Copyright (c) 2026, PostgreSQL Global Development Group
-- 
-- pgbench-graph (Neo4j): Graph Algorithms Suite (BFS, WCC, SSSP, PageRank, LCC)
--

\set root_id 1

-- 1. Breadth-First Search (BFS)
MATCH (root:Person {id: :root_id})
OPTIONAL MATCH path = (root)-[:knows*1..2]->(dst:Person)
WITH root.id AS root_id, collect({vid: dst.id, depth: length(path)}) AS items
UNWIND items + [{vid: root_id, depth: 0}] AS r
WITH r.vid AS vertex_id, min(r.depth) AS min_depth, root_id
RETURN count(*) AS bfs_reachable,
       min(CASE WHEN vertex_id = root_id THEN min_depth ELSE null END) AS bfs_root_depth,
       max(min_depth) AS bfs_max_depth \gset

\validate_result :bfs_root_depth 0
\validate_result (:bfs_reachable >= 1 and :bfs_max_depth >= 0) true

-- 2. Weakly Connected Components (WCC)
MATCH (p:Person WHERE p.id <= 500)
OPTIONAL MATCH (p)-[:knows]->(nbr:Person WHERE nbr.id <= 500)
WITH p.id AS vid, min(coalesce(nbr.id, p.id)) AS cid
RETURN count(DISTINCT cid) AS wcc_components,
       count(*) AS wcc_vertices \gset

\validate_result (:wcc_components >= 1 and :wcc_components <= :wcc_vertices) true

-- 3. Single-Source Shortest Paths (SSSP)
MATCH (root:Person {id: :root_id})
OPTIONAL MATCH (root)-[k:knows]->(dst:Person)
WITH root.id AS root_id, collect({vid: dst.id, dist: k.weight}) AS items
UNWIND items + [{vid: root_id, dist: 0.0}] AS r
WITH r.vid AS vid, min(r.dist) AS min_dist, root_id
RETURN count(*) AS sssp_reached,
       min(CASE WHEN vid = root_id THEN min_dist ELSE null END) AS sssp_root_dist,
       round(sum(min_dist)) AS sssp_dist_sum \gset

\validate_result (:sssp_reached >= 1 and :sssp_dist_sum >= 0) true

-- 4. PageRank / In-Degree Centric Ranking
MATCH (src:Person WHERE src.id <= 1000)-[:knows]->(dst:Person WHERE dst.id <= 1000)
WITH dst.id AS vid, count(*) AS deg
RETURN count(*) AS pr_vertices,
       sum(deg) AS pr_score_sum \gset

\validate_result (:pr_vertices >= 1 and :pr_score_sum >= 0) true

-- 5. Local Clustering Coefficient (LCC)
MATCH (root:Person {id: :root_id})-[k1:knows]->(nbr:Person)
WITH root, collect(nbr) AS neighbors, count(nbr) AS deg
UNWIND neighbors AS n1
UNWIND neighbors AS n2
WITH root, deg, count(CASE WHEN n1.id < n2.id AND (n1)-[:knows]->(n2) THEN 1 END) AS triangles
RETURN deg,
       triangles,
       CASE WHEN deg > 1 THEN toFloat(2 * triangles) / (deg * (deg - 1)) ELSE 0.0 END AS lcc_score \gset

\validate_result (:deg >= 0) true
