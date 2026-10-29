--
-- Copyright (c) 2026, PostgreSQL Global Development Group
-- 
-- pgbench-graph (Neo4j): Transactional Edge & Message Insertion
--

\set src_id random_zipfian(1, 200 * :scale, 1.2)
\set dst_id random(1, 200 * :scale)
\set new_msg_id (1700100000000000 + :client_id * 100000000 + random(1, 99999999))

MATCH (p1:Person {id: :src_id}), (p2:Person {id: :dst_id})
MERGE (p1)-[r1:knows]->(p2)
ON CREATE SET r1.weight = 1.0, r1.created_ts = timestamp()
MERGE (p2)-[r2:knows]->(p1)
ON CREATE SET r2.weight = 1.0, r2.created_ts = timestamp()
CREATE (m:Message {
    id: :new_msg_id,
    creator_id: :src_id,
    content: 'graph-stream-post',
    created_ts: timestamp()
})-[:hasCreator]->(p1)
RETURN m.id AS edge_p_msg_id \gset

\if reservoir_count('persons') >= 0
\reservoir_push persons :src_id
\reservoir_push persons :dst_id
\endif
\advance_watermark :edge_p_msg_id
