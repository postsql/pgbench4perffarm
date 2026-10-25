--
-- pgbench-vec: Streaming Vector Upsert & Causal Watermark Advancement
--

\set dim 8
\set new_id random(1, 200 * :scale)
\set cat random(0, 9)
\set tag1 random(0, 4)
\set tag2 random(0, 4)
\set v clustered_vector(:dim, 10, 0.15, 42, :cat)
\set iv random_unit_vector(:dim)

INSERT INTO pgbench_vec_items (id, category, tags, payload, embedding, image_embedding)
VALUES (:new_id, :cat, ARRAY[:tag1, :tag2], 'streamed-item', pgbench_vec_parse(:v), pgbench_vec_parse(:iv))
ON CONFLICT (id) DO UPDATE
   SET embedding = EXCLUDED.embedding,
       image_embedding = EXCLUDED.image_embedding,
       category = EXCLUDED.category
RETURNING id \gset ins_

\if reservoir_count('vec_ids') >= 0
\reservoir_push vec_ids :ins_id
\endif
\advance_watermark :ins_id
