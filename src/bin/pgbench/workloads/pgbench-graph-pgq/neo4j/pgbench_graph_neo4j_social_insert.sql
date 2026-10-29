--
-- Copyright (c) 2026, PostgreSQL Global Development Group
-- 
-- pgbench-graph (Neo4j): Social Graph Interactive Inserts (IU1 - IU8)
--

\set op_type random(1, 8)
\set event_ts (1700100000000000 + :client_id * 1000 + random(1, 50000))
\set new_unique_id (1700100000000000 + :client_id * 100000000 + random(1, 99999999))
\set p1_id random_zipfian(1, 200 * :scale, 1.2)
\set p2_id community_correlated_edge(:p1_id, 200 * :scale, 10, 0.75, 42)
\set forum_id random(1, 20 * :scale)
\set tag_id random(1, 100)

\begin_dependent_write :event_ts CHANNEL 0

\if :op_type = 1
-- IU1: Add Person
MATCH (pl:Place {id: 17 + (:p1_id % 50)})
CREATE (p:Person {
    id: :new_unique_id,
    first_name: 'NewPerson',
    last_name: 'Stream',
    gender: 'female',
    city_id: pl.id,
    company_id: 1,
    university_id: 1,
    created_ts: :event_ts
})-[:isLocatedIn]->(pl)
RETURN p.id AS new_person_id \gset

\if reservoir_count('persons') >= 0
\reservoir_push persons :new_person_id
\endif

\elif :op_type = 2
-- IU2: Add Post Like
MATCH (p:Person {id: :p1_id}), (m:Message {id: 1 + ((:p2_id - 1) * 2)})
MERGE (p)-[r:likes]->(m)
ON CREATE SET r.created_ts = :event_ts;

\elif :op_type = 3
-- IU3: Add Comment Like
MATCH (p:Person {id: :p1_id}), (m:Message {id: (:p2_id - 1) * 2 + 2})
MERGE (p)-[r:likes]->(m)
ON CREATE SET r.created_ts = :event_ts;

\elif :op_type = 4
-- IU4: Add Forum
MATCH (p:Person {id: :p1_id})
CREATE (f:Forum {
    id: :new_unique_id,
    title: 'StreamForum',
    moderator_id: :p1_id,
    created_ts: :event_ts
})-[:hasModerator]->(p),
(f)-[:hasMember {joined_ts: :event_ts}]->(p)
RETURN f.id AS new_forum_id;

\elif :op_type = 5
-- IU5: Add Forum Membership
MATCH (f:Forum {id: :forum_id}), (p:Person {id: :p1_id})
MERGE (f)-[r:hasMember]->(p)
ON CREATE SET r.joined_ts = :event_ts;

\elif :op_type = 6
-- IU6: Add Post
MATCH (p:Person {id: :p1_id}), (f:Forum {id: :forum_id})
CREATE (m:Message {
    id: :new_unique_id,
    creator_id: :p1_id,
    forum_id: :forum_id,
    content: 'IU6 Stream Post',
    length: 28,
    created_ts: :event_ts
})-[:hasCreator]->(p),
(f)-[:containerOf {created_ts: :event_ts}]->(m)
RETURN m.id AS new_msg_id \gset

\if reservoir_count('messages') >= 0
\reservoir_push messages :new_msg_id
\endif

\elif :op_type = 7
-- IU7: Add Comment Reply
MATCH (p:Person {id: :p1_id}), (parent:Message {id: 1 + ((:p2_id - 1) * 2)})
CREATE (m:Message {
    id: :new_unique_id,
    creator_id: :p1_id,
    parent_message_id: parent.id,
    content: 'IU7 Stream Reply',
    length: 29,
    created_ts: :event_ts
})-[:hasCreator]->(p),
(m)-[:replyOf {created_ts: :event_ts}]->(parent)
RETURN m.id AS new_msg_id \gset

\if reservoir_count('messages') >= 0
\reservoir_push messages :new_msg_id
\endif

\else
-- IU8: Add Bidirectional Friendship
MATCH (p1:Person {id: :p1_id}), (p2:Person {id: :p2_id})
MERGE (p1)-[r1:knows]->(p2)
ON CREATE SET r1.weight = 1.0, r1.created_ts = :event_ts
MERGE (p2)-[r2:knows]->(p1)
ON CREATE SET r2.weight = 1.0, r2.created_ts = :event_ts;

\endif

\end_dependent_write :event_ts CHANNEL 0
