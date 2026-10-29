--
-- Copyright (c) 2026, PostgreSQL Global Development Group
-- 
-- pgbench-graph (Neo4j): Social Graph Interactive Complex Reads (IC1-IC5, IC9, IC12, IC13)
--

\if reservoir_count('persons') >= 0
\reservoir_sample persons person_id random_zipfian(1, 200 * :scale, 1.2)
\else
\set person_id random_zipfian(1, 200 * :scale, 1.2)
\endif

\set other_person_id community_correlated_edge(:person_id, 200 * :scale, 10, 0.50, 77)
\set cutoff_ts 1700090000000000
\set window_start_ts 1700000000000000

-- IC1 & IC2: 1..2 hop friends
MATCH (src:Person {id: :person_id})-[k:knows*1..2]->(f:Person)
WHERE f.id <> :person_id
WITH f.id AS friend_id, f.last_name AS last_name, size(k) AS dist
RETURN friend_id AS disc_person_id, last_name, min(dist) AS min_dist
ORDER BY min_dist ASC, last_name ASC, friend_id ASC
LIMIT 10 \gset_col disc_person_id

\if reservoir_count('persons') >= 0
\reservoir_push persons :disc_person_id
\endif

MATCH (src:Person {id: :person_id})-[:knows]->(f:Person)<-[:hasCreator]-(msg:Message)
WHERE msg.created_ts <= :cutoff_ts
RETURN msg.id AS disc_msg_id, msg.created_ts AS created_ts
ORDER BY created_ts DESC
LIMIT 10 \gset_col disc_msg_id

\if reservoir_count('messages') >= 0
\reservoir_push messages :disc_msg_id
\endif

-- IC3 & IC4: Popular tags among 1..2 hop friends
MATCH (src:Person {id: :person_id})-[:knows*1..2]->(f:Person)<-[:hasCreator]-(m:Message)-[:hasTag]->(t:Tag)
WHERE f.id <> :person_id AND m.parent_message_id IS NULL AND m.created_ts >= :window_start_ts AND m.created_ts <= :cutoff_ts
WITH DISTINCT f.id AS fid, m.id AS msg_id, t.id AS tag_id, t.name AS tag_name
RETURN tag_name, count(*) AS post_count
ORDER BY post_count DESC, tag_name ASC
LIMIT 10;

-- IC5 & IC9: Forums joined by friends-of-friends
MATCH (src:Person {id: :person_id})-[:knows*2..2]->(f2:Person)<-[:hasMember]-(f:Forum)
WHERE f2.id <> :person_id
RETURN f.id AS forum_id, f.title AS title, count(DISTINCT f2) AS fof_member_count
ORDER BY fof_member_count DESC, forum_id ASC
LIMIT 10;

-- IC13: Shortest path between two persons
MATCH path = shortestPath((p1:Person {id: :person_id})-[:knows*..5]-(p2:Person {id: :other_person_id}))
RETURN length(path) AS shortest_path_dist;
