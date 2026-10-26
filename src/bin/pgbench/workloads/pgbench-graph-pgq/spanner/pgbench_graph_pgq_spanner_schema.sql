DROP PROPERTY GRAPH IF EXISTS financial_graph;
DROP PROPERTY GRAPH IF EXISTS social_graph;
DROP PROPERTY GRAPH IF EXISTS finbench_graph;
DROP PROPERTY GRAPH IF EXISTS snb_graph;

DROP INDEX IF EXISTS pgbench_graph_tags_tagclass_idx;
DROP INDEX IF EXISTS pgbench_graph_forums_moderator_idx;
DROP INDEX IF EXISTS pgbench_graph_messages_created_ts_idx;
DROP INDEX IF EXISTS pgbench_graph_places_part_of_idx;
DROP INDEX IF EXISTS pgbench_graph_persons_city_idx;
DROP INDEX IF EXISTS pgbench_fin_sign_ins_acct_idx;
DROP INDEX IF EXISTS pgbench_graph_forum_members_person_idx;
DROP INDEX IF EXISTS pgbench_fin_loans_borrower_idx;
DROP INDEX IF EXISTS pgbench_fin_transfers_dst_ts_idx;
DROP INDEX IF EXISTS pgbench_fin_transfers_src_ts_idx;
DROP INDEX IF EXISTS pgbench_graph_person_tags_tag_idx;
DROP INDEX IF EXISTS pgbench_graph_msg_tags_tag_idx;
DROP INDEX IF EXISTS pgbench_graph_likes_msg_idx;
DROP INDEX IF EXISTS pgbench_graph_messages_forum_ts_idx;
DROP INDEX IF EXISTS pgbench_graph_messages_parent_idx;
DROP INDEX IF EXISTS pgbench_graph_messages_creator_ts_idx;
DROP INDEX IF EXISTS pgbench_graph_knows_dst_src_idx;

DROP TABLE IF EXISTS pgbench_fin_sign_ins;
DROP TABLE IF EXISTS pgbench_fin_mediums;
DROP TABLE IF EXISTS pgbench_fin_loans;
DROP TABLE IF EXISTS pgbench_fin_transfers;
DROP TABLE IF EXISTS pgbench_fin_accounts;

DROP TABLE IF EXISTS pgbench_graph_message_tags;
DROP TABLE IF EXISTS pgbench_graph_likes;
DROP TABLE IF EXISTS pgbench_graph_messages;
DROP TABLE IF EXISTS pgbench_graph_forum_members;
DROP TABLE IF EXISTS pgbench_graph_forums;
DROP TABLE IF EXISTS pgbench_graph_person_tags;
DROP TABLE IF EXISTS pgbench_graph_knows;
DROP TABLE IF EXISTS pgbench_graph_persons;
DROP TABLE IF EXISTS pgbench_graph_tags;
DROP TABLE IF EXISTS pgbench_graph_places;

DROP SEQUENCE IF EXISTS pgbench_fin_loan_seq;
DROP SEQUENCE IF EXISTS pgbench_fin_transfer_seq;
DROP SEQUENCE IF EXISTS pgbench_graph_msg_seq;
DROP SEQUENCE IF EXISTS pgbench_graph_forum_seq;
DROP SEQUENCE IF EXISTS pgbench_graph_person_seq;

CREATE SEQUENCE pgbench_graph_person_seq OPTIONS (
    sequence_kind = 'bit_reversed_positive',
    skip_range_min = 1,
    skip_range_max = 1000000000
);

CREATE SEQUENCE pgbench_graph_forum_seq OPTIONS (
    sequence_kind = 'bit_reversed_positive',
    skip_range_min = 1,
    skip_range_max = 1000000000
);

CREATE SEQUENCE pgbench_graph_msg_seq OPTIONS (
    sequence_kind = 'bit_reversed_positive',
    skip_range_min = 1,
    skip_range_max = 1000000000
);

CREATE SEQUENCE pgbench_fin_transfer_seq OPTIONS (
    sequence_kind = 'bit_reversed_positive',
    skip_range_min = 1,
    skip_range_max = 1000000000
);

CREATE SEQUENCE pgbench_fin_loan_seq OPTIONS (
    sequence_kind = 'bit_reversed_positive',
    skip_range_min = 1,
    skip_range_max = 1000000000
);

CREATE TABLE pgbench_graph_places (
    id INT64 NOT NULL,
    name STRING(MAX) NOT NULL,
    type STRING(MAX) NOT NULL,
    part_of_id INT64,
) PRIMARY KEY (id);

CREATE TABLE pgbench_graph_tags (
    id INT64 NOT NULL,
    name STRING(MAX) NOT NULL,
    tagclass_id INT64 NOT NULL,
) PRIMARY KEY (id);

CREATE TABLE pgbench_graph_persons (
    id INT64 NOT NULL DEFAULT (GET_NEXT_SEQUENCE_VALUE(SEQUENCE pgbench_graph_person_seq)),
    first_name STRING(MAX) NOT NULL,
    last_name STRING(MAX) NOT NULL,
    gender STRING(MAX) NOT NULL DEFAULT ('female'),
    city_id INT64 NOT NULL,
    company_id INT64 NOT NULL DEFAULT (1),
    university_id INT64 NOT NULL DEFAULT (1),
    created_ts INT64 NOT NULL,
) PRIMARY KEY (id);

CREATE TABLE pgbench_graph_knows (
    src_id INT64 NOT NULL,
    dst_id INT64 NOT NULL,
    weight FLOAT64 NOT NULL DEFAULT (1.0),
    created_ts INT64 NOT NULL,
) PRIMARY KEY (src_id, dst_id);

CREATE TABLE pgbench_graph_person_tags (
    person_id INT64 NOT NULL,
    tag_id INT64 NOT NULL,
) PRIMARY KEY (person_id, tag_id);

CREATE TABLE pgbench_graph_forums (
    id INT64 NOT NULL DEFAULT (GET_NEXT_SEQUENCE_VALUE(SEQUENCE pgbench_graph_forum_seq)),
    title STRING(MAX) NOT NULL,
    moderator_id INT64 NOT NULL,
    created_ts INT64 NOT NULL,
) PRIMARY KEY (id);

CREATE TABLE pgbench_graph_forum_members (
    forum_id INT64 NOT NULL,
    person_id INT64 NOT NULL,
    joined_ts INT64 NOT NULL,
) PRIMARY KEY (forum_id, person_id);

CREATE TABLE pgbench_graph_messages (
    id INT64 NOT NULL DEFAULT (GET_NEXT_SEQUENCE_VALUE(SEQUENCE pgbench_graph_msg_seq)),
    creator_id INT64 NOT NULL,
    forum_id INT64,
    parent_message_id INT64,
    country_id INT64 NOT NULL DEFAULT (1),
    content STRING(MAX) NOT NULL,
    length INT64 NOT NULL DEFAULT (24),
    created_ts INT64 NOT NULL,
) PRIMARY KEY (id);

CREATE TABLE pgbench_graph_likes (
    person_id INT64 NOT NULL,
    message_id INT64 NOT NULL,
    created_ts INT64 NOT NULL,
) PRIMARY KEY (person_id, message_id);

CREATE TABLE pgbench_graph_message_tags (
    message_id INT64 NOT NULL,
    tag_id INT64 NOT NULL,
) PRIMARY KEY (message_id, tag_id);

CREATE TABLE pgbench_fin_accounts (
    id INT64 NOT NULL,
    owner_id INT64 NOT NULL,
    owner_type STRING(MAX) NOT NULL DEFAULT ('person'),
    is_blocked BOOL NOT NULL DEFAULT (FALSE),
    balance NUMERIC NOT NULL DEFAULT (NUMERIC '10000.00'),
    created_ts INT64 NOT NULL,
) PRIMARY KEY (id);

CREATE TABLE pgbench_fin_transfers (
    id INT64 NOT NULL DEFAULT (GET_NEXT_SEQUENCE_VALUE(SEQUENCE pgbench_fin_transfer_seq)),
    src_acct INT64 NOT NULL,
    dst_acct INT64 NOT NULL,
    amount NUMERIC NOT NULL,
    created_ts INT64 NOT NULL,
) PRIMARY KEY (id);

CREATE TABLE pgbench_fin_loans (
    id INT64 NOT NULL DEFAULT (GET_NEXT_SEQUENCE_VALUE(SEQUENCE pgbench_fin_loan_seq)),
    borrower_id INT64 NOT NULL,
    acct_id INT64 NOT NULL,
    loan_amount NUMERIC NOT NULL,
    balance NUMERIC NOT NULL,
    created_ts INT64 NOT NULL,
) PRIMARY KEY (id);

CREATE TABLE pgbench_fin_mediums (
    id INT64 NOT NULL,
    type STRING(MAX) NOT NULL,
    is_blocked BOOL NOT NULL DEFAULT (FALSE),
    risk_score INT64 NOT NULL DEFAULT (10),
) PRIMARY KEY (id);

CREATE TABLE pgbench_fin_sign_ins (
    medium_id INT64 NOT NULL,
    acct_id INT64 NOT NULL,
    created_ts INT64 NOT NULL,
) PRIMARY KEY (medium_id, acct_id, created_ts);

CREATE INDEX pgbench_graph_knows_dst_src_idx
    ON pgbench_graph_knows (dst_id, src_id);

CREATE INDEX pgbench_graph_messages_creator_ts_idx
    ON pgbench_graph_messages (creator_id, created_ts DESC);

CREATE NULL_FILTERED INDEX pgbench_graph_messages_parent_idx
    ON pgbench_graph_messages (parent_message_id);

CREATE INDEX pgbench_graph_messages_forum_ts_idx
    ON pgbench_graph_messages (forum_id, created_ts DESC);

CREATE INDEX pgbench_graph_likes_msg_idx
    ON pgbench_graph_likes (message_id, person_id);

CREATE INDEX pgbench_graph_msg_tags_tag_idx
    ON pgbench_graph_message_tags (tag_id, message_id);

CREATE INDEX pgbench_graph_person_tags_tag_idx
    ON pgbench_graph_person_tags (tag_id, person_id);

CREATE INDEX pgbench_fin_transfers_src_ts_idx
    ON pgbench_fin_transfers (src_acct, created_ts, dst_acct);

CREATE INDEX pgbench_fin_transfers_dst_ts_idx
    ON pgbench_fin_transfers (dst_acct, created_ts, src_acct);

CREATE INDEX pgbench_fin_loans_borrower_idx
    ON pgbench_fin_loans (borrower_id, acct_id);

CREATE INDEX pgbench_graph_forum_members_person_idx
    ON pgbench_graph_forum_members (person_id, forum_id);

CREATE INDEX pgbench_fin_sign_ins_acct_idx
    ON pgbench_fin_sign_ins (acct_id, medium_id, created_ts);

CREATE INDEX pgbench_graph_persons_city_idx
    ON pgbench_graph_persons (city_id, id);

CREATE INDEX pgbench_graph_places_part_of_idx
    ON pgbench_graph_places (part_of_id, type, id);

CREATE INDEX pgbench_graph_messages_created_ts_idx
    ON pgbench_graph_messages (created_ts DESC, id);

CREATE INDEX pgbench_graph_forums_moderator_idx
    ON pgbench_graph_forums (moderator_id, id);

CREATE INDEX pgbench_graph_tags_tagclass_idx
    ON pgbench_graph_tags (tagclass_id, id);

CREATE OR REPLACE PROPERTY GRAPH social_graph
    NODE TABLES (
        pgbench_graph_places KEY (id)
            LABEL Place PROPERTIES ALL COLUMNS,
        pgbench_graph_tags KEY (id)
            LABEL Tag PROPERTIES ALL COLUMNS,
        pgbench_graph_persons KEY (id)
            LABEL Person PROPERTIES ALL COLUMNS,
        pgbench_graph_forums KEY (id)
            LABEL Forum PROPERTIES ALL COLUMNS,
        pgbench_graph_messages KEY (id)
            LABEL Message PROPERTIES ALL COLUMNS
    )
    EDGE TABLES (
        pgbench_graph_knows KEY (src_id, dst_id)
            SOURCE KEY (src_id) REFERENCES pgbench_graph_persons (id)
            DESTINATION KEY (dst_id) REFERENCES pgbench_graph_persons (id)
            LABEL knows PROPERTIES ALL COLUMNS,
        pgbench_graph_person_tags KEY (person_id, tag_id)
            SOURCE KEY (person_id) REFERENCES pgbench_graph_persons (id)
            DESTINATION KEY (tag_id) REFERENCES pgbench_graph_tags (id)
            LABEL hasInterest PROPERTIES ALL COLUMNS,
        pgbench_graph_forum_members KEY (forum_id, person_id)
            SOURCE KEY (forum_id) REFERENCES pgbench_graph_forums (id)
            DESTINATION KEY (person_id) REFERENCES pgbench_graph_persons (id)
            LABEL hasMember PROPERTIES ALL COLUMNS,
        pgbench_graph_likes KEY (person_id, message_id)
            SOURCE KEY (person_id) REFERENCES pgbench_graph_persons (id)
            DESTINATION KEY (message_id) REFERENCES pgbench_graph_messages (id)
            LABEL likes PROPERTIES ALL COLUMNS,
        pgbench_graph_message_tags KEY (message_id, tag_id)
            SOURCE KEY (message_id) REFERENCES pgbench_graph_messages (id)
            DESTINATION KEY (tag_id) REFERENCES pgbench_graph_tags (id)
            LABEL hasTag PROPERTIES ALL COLUMNS,
        pgbench_graph_messages AS msg_has_creator KEY (id)
            SOURCE KEY (id) REFERENCES pgbench_graph_messages (id)
            DESTINATION KEY (creator_id) REFERENCES pgbench_graph_persons (id)
            LABEL hasCreator PROPERTIES (id, creator_id, created_ts),
        pgbench_graph_messages AS msg_reply_of KEY (id)
            SOURCE KEY (id) REFERENCES pgbench_graph_messages (id)
            DESTINATION KEY (parent_message_id) REFERENCES pgbench_graph_messages (id)
            LABEL replyOf PROPERTIES (id, parent_message_id, created_ts),
        pgbench_graph_messages AS msg_in_forum KEY (id)
            SOURCE KEY (forum_id) REFERENCES pgbench_graph_forums (id)
            DESTINATION KEY (id) REFERENCES pgbench_graph_messages (id)
            LABEL containerOf PROPERTIES (id, forum_id, created_ts),
        pgbench_graph_forums AS forum_has_moderator KEY (id)
            SOURCE KEY (id) REFERENCES pgbench_graph_forums (id)
            DESTINATION KEY (moderator_id) REFERENCES pgbench_graph_persons (id)
            LABEL hasModerator PROPERTIES (id, moderator_id, created_ts),
        pgbench_graph_persons AS person_located_in KEY (id)
            SOURCE KEY (id) REFERENCES pgbench_graph_persons (id)
            DESTINATION KEY (city_id) REFERENCES pgbench_graph_places (id)
            LABEL isLocatedIn PROPERTIES (id, city_id),
        pgbench_graph_places AS place_part_of KEY (id)
            SOURCE KEY (id) REFERENCES pgbench_graph_places (id)
            DESTINATION KEY (part_of_id) REFERENCES pgbench_graph_places (id)
            LABEL isPartOf PROPERTIES (id, part_of_id)
    );

CREATE OR REPLACE PROPERTY GRAPH financial_graph
    NODE TABLES (
        pgbench_fin_accounts KEY (id)
            LABEL Account PROPERTIES ALL COLUMNS,
        pgbench_fin_mediums KEY (id)
            LABEL Medium PROPERTIES ALL COLUMNS,
        pgbench_fin_loans KEY (id)
            LABEL Loan PROPERTIES ALL COLUMNS
    )
    EDGE TABLES (
        pgbench_fin_transfers KEY (id)
            SOURCE KEY (src_acct) REFERENCES pgbench_fin_accounts (id)
            DESTINATION KEY (dst_acct) REFERENCES pgbench_fin_accounts (id)
            LABEL transfer PROPERTIES ALL COLUMNS,
        pgbench_fin_sign_ins KEY (medium_id, acct_id, created_ts)
            SOURCE KEY (medium_id) REFERENCES pgbench_fin_mediums (id)
            DESTINATION KEY (acct_id) REFERENCES pgbench_fin_accounts (id)
            LABEL signIn PROPERTIES ALL COLUMNS,
        pgbench_fin_loans AS loan_deposit KEY (id)
            SOURCE KEY (id) REFERENCES pgbench_fin_loans (id)
            DESTINATION KEY (acct_id) REFERENCES pgbench_fin_accounts (id)
            LABEL deposit PROPERTIES (id, borrower_id, acct_id, loan_amount, balance, created_ts)
    );
