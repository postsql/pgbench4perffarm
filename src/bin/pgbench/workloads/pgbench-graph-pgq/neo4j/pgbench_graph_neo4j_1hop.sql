--
-- Copyright (c) 2026, PostgreSQL Global Development Group
-- 
-- pgbench-graph (Neo4j): 1-Hop Neighborhood & Recent Messages Query
-- Implements 1-hop friend discovery and top-2 recent messages per friend in Cypher.
--

\if reservoir_count('persons') >= 0
\reservoir_sample persons person_id random_zipfian(1, 200 * :scale, 1.2)
\else
\set person_id random_zipfian(1, 200 * :scale, 1.2)
\endif

MATCH (src:Person {id: :person_id})-[k:knows]->(friend:Person)
CALL {
    WITH friend
    OPTIONAL MATCH (msg:Message)-[:hasCreator]->(friend)
    RETURN msg.id AS msg_id, msg.created_ts AS msg_created_ts
    ORDER BY msg.created_ts DESC
    LIMIT 2
}
RETURN friend.id AS friend_id,
       friend.first_name AS first_name,
       friend.last_name AS last_name,
       k.weight AS weight,
       msg_id,
       msg_created_ts
ORDER BY weight DESC, friend_id ASC
LIMIT 10;
