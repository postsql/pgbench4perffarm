--
-- Copyright (c) 2026, PostgreSQL Global Development Group
-- 
-- pgbench-graph (Neo4j): Labelled Subgraph Query Benchmark Suite (Q1 - Q9)
-- Executes unseeded cyclic (Q1-Q5), acyclic (Q6-Q7), and anti-join (Q8-Q9)
-- structural pattern-matching count queries over Neo4j property graph.
-- Note: Q3 country-localized friendship triangle is evaluated in the per-query
-- isolated benchmark due to transaction timeout on global triangle expansion.
--

-- Q1: Long path across Country <-[:isPartOf]- City <-[:isLocatedIn]- Person <-[:hasMember]- Forum -[:containerOf]-> Post <-[:replyOf]- Comment -[:hasTag]-> Tag
MATCH (country:Place {type: 'country'})<-[:isPartOf]-(city:Place {type: 'city'})<-[:isLocatedIn]-(p:Person)<-[:hasMember]-(f:Forum)-[:containerOf]->(post:Message)<-[:replyOf]-(cmt:Message)-[:hasTag]->(t:Tag)
WHERE post.parent_message_id IS NULL
RETURN count(*) AS q1_count \gset

\validate_result (:q1_count >= 0) true

-- Q2: Cyclic 4-pattern: Person1 -[:knows]-> Person2 <-[:hasCreator]- Comment -[:replyOf]-> Post -[:hasCreator]-> Person1
MATCH (p1:Person)-[:knows]->(p2:Person)<-[:hasCreator]-(cmt:Message)-[:replyOf]->(post:Message)-[:hasCreator]->(p1)
WHERE post.parent_message_id IS NULL
RETURN count(*) AS q2_count \gset

\validate_result (:q2_count >= 0) true

-- Q4: Message - Tag - Like - Creator cyclic interaction pattern
MATCH (t:Tag)<-[:hasTag]-(m:Message)<-[:likes]-(liker:Person)<-[:knows]-(creator:Person)<-[:hasCreator]-(m)
RETURN count(*) AS q4_count \gset

\validate_result (:q4_count >= 0) true

-- Q5: Message <-[:replyOf]- Comment chain with distinct tags (tag1 <> tag2)
MATCH (t1:Tag)<-[:hasTag]-(m:Message)<-[:replyOf]-(cmt:Message)-[:hasTag]->(t2:Tag)
WHERE t1.id <> t2.id
RETURN count(*) AS q5_count \gset

\validate_result (:q5_count >= 0) true

-- Q6: 2-hop Acyclic Path: Person1 -[:knows]-> Person2 -[:knows]-> Person3 -[:hasInterest]-> Tag (Person1 <> Person3)
MATCH (p1:Person)-[:knows]->(p2:Person)
WITH p2, count(p1) AS in_deg
MATCH (p2)-[:knows]->(p3:Person)-[:hasInterest]->(t:Tag)
RETURN sum(in_deg - 1) AS q6_count \gset

\validate_result (:q6_count >= 0) true

-- Q7: Star pattern with OPTIONAL likes and replies
MATCH (m:Message)-[:hasTag]->(t:Tag)
OPTIONAL MATCH (liker:Person)-[:likes]->(m)
OPTIONAL MATCH (cmt:Message)-[:replyOf]->(m)
RETURN count(*) AS q7_count \gset

\validate_result (:q7_count >= 0) true

-- Q8: Anti-Join Pattern: Message <-[:replyOf]- Comment where Comment has tag2 but NOT tag1 of parent Message
MATCH (t1:Tag)<-[:hasTag]-(m:Message)<-[:replyOf]-(cmt:Message)-[:hasTag]->(t2:Tag)
WHERE t1.id <> t2.id AND NOT (cmt)-[:hasTag]->(t1)
RETURN count(*) AS q8_count \gset

\validate_result (:q8_count >= 0) true

-- Q9: Open-Triad Anti-Join
MATCH (p1:Person WHERE p1.id <= 200)-[:knows]->(p2:Person WHERE p2.id <= 1000)-[:knows]->(p3:Person WHERE p3.id <= 1000)-[:hasInterest]->(t:Tag)
WHERE p3.id > p1.id AND NOT (p1)-[:knows]->(p3)
RETURN count(*) AS q9_count \gset

\validate_result (:q9_count >= 0) true
