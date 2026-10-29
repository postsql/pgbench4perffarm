--
-- Copyright (c) 2026, PostgreSQL Global Development Group
-- 
-- pgbench-graph (Neo4j): Social Graph Interactive Short Reads (IS1 - IS7)
--

\if reservoir_count('persons') >= 0
\reservoir_sample persons person_id random_zipfian(1, 200 * :scale, 1.2)
\else
\set person_id random_zipfian(1, 200 * :scale, 1.2)
\endif

\if reservoir_count('messages') >= 0
\reservoir_sample messages message_id random(1, 400 * :scale)
\else
\set message_id random(1, 400 * :scale)
\endif

-- IS1: Profile of a person
MATCH (p:Person {id: :person_id})-[:isLocatedIn]->(pl:Place)
RETURN p.first_name AS first_name, p.last_name AS last_name, p.gender AS gender, pl.name AS city_name, p.created_ts AS created_ts;

-- IS2: Recent messages of a person & original root post info
MATCH (m:Message)-[:hasCreator]->(p:Person {id: :person_id})
OPTIONAL MATCH (m)-[:replyOf]->(root_p:Message)
RETURN m.id AS msg_id, m.content AS content, m.created_ts AS created_ts, coalesce(root_p.id, m.id) AS post_id, coalesce(root_p.creator_id, m.creator_id) AS post_author_id
ORDER BY m.created_ts DESC, m.id DESC
LIMIT 10;

-- IS3: Friends of a person
MATCH (src:Person {id: :person_id})-[k:knows]->(friend:Person)
RETURN friend.id AS friend_id, friend.first_name AS first_name, friend.last_name AS last_name, k.created_ts AS friendship_ts
ORDER BY friendship_ts DESC, friend_id ASC
LIMIT 20;

-- IS4 & IS5: Content and creator of a message
MATCH (m:Message {id: :message_id})-[:hasCreator]->(p:Person)
RETURN m.content AS content, m.created_ts AS created_ts, p.id AS author_id, p.first_name AS first_name, p.last_name AS last_name;

-- IS6: Forum and moderator of a message
MATCH (m:Message {id: :message_id})<-[:containerOf]-(f:Forum)-[:hasModerator]->(mod:Person)
RETURN f.id AS forum_id, f.title AS forum_title, mod.id AS moderator_id, mod.first_name AS first_name, mod.last_name AS last_name;

-- IS7: Comments on a message and author friendship status
MATCH (cmt:Message)-[:replyOf]->(m:Message {id: :message_id})
MATCH (cmt)-[:hasCreator]->(author:Person)
OPTIONAL MATCH (author)-[k:knows]->(m_author:Person)<-[:hasCreator]-(m)
RETURN cmt.id AS comment_id, cmt.content AS comment_content, cmt.created_ts AS comment_created_ts, author.id AS author_id, author.first_name AS author_first_name, author.last_name AS author_last_name, (k IS NOT NULL) AS is_friend
ORDER BY comment_created_ts DESC, author_id ASC;
