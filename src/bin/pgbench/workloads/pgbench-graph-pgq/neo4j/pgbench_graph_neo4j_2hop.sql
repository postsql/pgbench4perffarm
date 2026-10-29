--
-- Copyright (c) 2026, PostgreSQL Global Development Group
-- 
-- pgbench-graph (Neo4j): 2-Hop Friend-of-Friend Discovery & Scoring
-- Implements 2-hop friend-of-friend discovery excluding direct friends in Cypher.
--

\if reservoir_count('persons') >= 0
\reservoir_sample persons person_id random_zipfian(1, 200 * :scale, 1.2)
\else
\set person_id random_zipfian(1, 200 * :scale, 1.2)
\endif

MATCH (p1:Person {id: :person_id})-[:knows]->(p2:Person)-[k2:knows]->(p3:Person)
WHERE p3.id <> :person_id AND NOT (p1)-[:knows]->(p3)
RETURN p3.id AS id,
       p3.first_name AS first_name,
       p3.last_name AS last_name,
       p3.city_id AS city_id,
       count(*) AS mutual_friends,
       sum(k2.weight) AS total_weight
ORDER BY mutual_friends DESC, total_weight DESC, id ASC
LIMIT 10;
