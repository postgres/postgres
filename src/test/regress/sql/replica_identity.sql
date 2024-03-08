CREATE TABLE test_replica_identity (
       id serial primary key,
       keya text not null,
       keyb text not null,
       nonkey text,
       CONSTRAINT test_replica_identity_unique_defer UNIQUE (keya, keyb) DEFERRABLE,
       CONSTRAINT test_replica_identity_unique_nondefer UNIQUE (keya, keyb)
) ;

CREATE TABLE test_replica_identity_othertable (id serial primary key);
CREATE TABLE test_replica_identity_t3 (id serial constraint pk primary key deferrable);

CREATE INDEX test_replica_identity_keyab ON test_replica_identity (keya, keyb);
CREATE UNIQUE INDEX test_replica_identity_keyab_key ON test_replica_identity (keya, keyb);
CREATE UNIQUE INDEX test_replica_identity_nonkey ON test_replica_identity (keya, nonkey);
CREATE INDEX test_replica_identity_hash ON test_replica_identity USING hash (nonkey);
CREATE UNIQUE INDEX test_replica_identity_expr ON test_replica_identity (keya, keyb, (3));
CREATE UNIQUE INDEX test_replica_identity_partial ON test_replica_identity (keya, keyb) WHERE keyb != '3';

-- default is 'd'/DEFAULT for user created tables
SELECT relreplident FROM pg_class WHERE oid = 'test_replica_identity'::regclass;
-- but 'none' for system tables
SELECT relreplident FROM pg_class WHERE oid = 'pg_class'::regclass;
SELECT relreplident FROM pg_class WHERE oid = 'pg_constraint'::regclass;

----
-- Make sure we detect ineligible indexes
----

-- fail, not unique
ALTER TABLE test_replica_identity REPLICA IDENTITY USING INDEX test_replica_identity_keyab;
-- fail, not a candidate key, nullable column
ALTER TABLE test_replica_identity REPLICA IDENTITY USING INDEX test_replica_identity_nonkey;
-- fail, hash indexes cannot do uniqueness
ALTER TABLE test_replica_identity REPLICA IDENTITY USING INDEX test_replica_identity_hash;
-- fail, expression index
ALTER TABLE test_replica_identity REPLICA IDENTITY USING INDEX test_replica_identity_expr;
-- fail, partial index
ALTER TABLE test_replica_identity REPLICA IDENTITY USING INDEX test_replica_identity_partial;
-- fail, not our index
ALTER TABLE test_replica_identity REPLICA IDENTITY USING INDEX test_replica_identity_othertable_pkey;
-- fail, deferrable
ALTER TABLE test_replica_identity REPLICA IDENTITY USING INDEX test_replica_identity_unique_defer;
-- fail, deferrable
ALTER TABLE test_replica_identity_t3 REPLICA IDENTITY USING INDEX pk;

SELECT relreplident FROM pg_class WHERE oid = 'test_replica_identity'::regclass;

----
-- Make sure index cases succeed
----

-- succeed, primary key
ALTER TABLE test_replica_identity REPLICA IDENTITY USING INDEX test_replica_identity_pkey;
SELECT relreplident FROM pg_class WHERE oid = 'test_replica_identity'::regclass;
\d test_replica_identity

-- succeed, nondeferrable unique constraint over nonnullable cols
ALTER TABLE test_replica_identity REPLICA IDENTITY USING INDEX test_replica_identity_unique_nondefer;

-- succeed unique index over nonnullable cols
ALTER TABLE test_replica_identity REPLICA IDENTITY USING INDEX test_replica_identity_keyab_key;
ALTER TABLE test_replica_identity REPLICA IDENTITY USING INDEX test_replica_identity_keyab_key;
SELECT relreplident FROM pg_class WHERE oid = 'test_replica_identity'::regclass;
\d test_replica_identity
SELECT count(*) FROM pg_index WHERE indrelid = 'test_replica_identity'::regclass AND indisreplident;

----
-- Make sure non index cases work
----
ALTER TABLE test_replica_identity REPLICA IDENTITY DEFAULT;
SELECT relreplident FROM pg_class WHERE oid = 'test_replica_identity'::regclass;
SELECT count(*) FROM pg_index WHERE indrelid = 'test_replica_identity'::regclass AND indisreplident;

ALTER TABLE test_replica_identity REPLICA IDENTITY FULL;
SELECT relreplident FROM pg_class WHERE oid = 'test_replica_identity'::regclass;
\d+ test_replica_identity
ALTER TABLE test_replica_identity REPLICA IDENTITY NOTHING;
SELECT relreplident FROM pg_class WHERE oid = 'test_replica_identity'::regclass;

---
-- Test that ALTER TABLE rewrite preserves nondefault replica identity
---

-- constraint variant
CREATE TABLE test_replica_identity2 (id int UNIQUE NOT NULL);
ALTER TABLE test_replica_identity2 REPLICA IDENTITY USING INDEX test_replica_identity2_id_key;
\d test_replica_identity2
ALTER TABLE test_replica_identity2 ALTER COLUMN id TYPE bigint;
\d test_replica_identity2

-- straight index variant
CREATE TABLE test_replica_identity3 (id int NOT NULL);
CREATE UNIQUE INDEX test_replica_identity3_id_key ON test_replica_identity3 (id);
ALTER TABLE test_replica_identity3 REPLICA IDENTITY USING INDEX test_replica_identity3_id_key;
\d test_replica_identity3
ALTER TABLE test_replica_identity3 ALTER COLUMN id TYPE bigint;
\d test_replica_identity3

-- ALTER TABLE DROP NOT NULL is not allowed for columns part of an index
-- used as replica identity.
ALTER TABLE test_replica_identity3 ALTER COLUMN id DROP NOT NULL;
-- but it's OK when the identity is FULL
ALTER TABLE test_replica_identity3 REPLICA IDENTITY FULL;
ALTER TABLE test_replica_identity3 ALTER COLUMN id DROP NOT NULL;

--
-- Test that replica identity can be set on an index that's not yet valid.
-- (This matches the way pg_dump will try to dump a partitioned table.)
--
CREATE TABLE test_replica_identity4(id integer NOT NULL) PARTITION BY LIST (id);
CREATE TABLE test_replica_identity4_1(id integer NOT NULL);
ALTER TABLE ONLY test_replica_identity4
  ATTACH PARTITION test_replica_identity4_1 FOR VALUES IN (1);
ALTER TABLE ONLY test_replica_identity4
  ADD CONSTRAINT test_replica_identity4_pkey PRIMARY KEY (id);
ALTER TABLE ONLY test_replica_identity4
  REPLICA IDENTITY USING INDEX test_replica_identity4_pkey;
ALTER TABLE ONLY test_replica_identity4_1
  ADD CONSTRAINT test_replica_identity4_1_pkey PRIMARY KEY (id);
\d+ test_replica_identity4
ALTER INDEX test_replica_identity4_pkey
  ATTACH PARTITION test_replica_identity4_1_pkey;
\d+ test_replica_identity4

-- Dropping the primary key is not allowed if that would leave the replica
-- identity as nullable
CREATE TABLE test_replica_identity5 (a int not null, b int, c int,
	PRIMARY KEY (b, c));
CREATE UNIQUE INDEX test_replica_identity5_a_b_key ON test_replica_identity5 (a, b);
ALTER TABLE test_replica_identity5 REPLICA IDENTITY USING INDEX test_replica_identity5_a_b_key;
ALTER TABLE test_replica_identity5 DROP CONSTRAINT test_replica_identity5_pkey;
ALTER TABLE test_replica_identity5 ALTER b SET NOT NULL;
ALTER TABLE test_replica_identity5 DROP CONSTRAINT test_replica_identity5_pkey;
ALTER TABLE test_replica_identity5 ALTER b DROP NOT NULL;

DROP TABLE test_replica_identity;
DROP TABLE test_replica_identity2;
DROP TABLE test_replica_identity3;
DROP TABLE test_replica_identity4;
DROP TABLE test_replica_identity5;
DROP TABLE test_replica_identity_othertable;
DROP TABLE test_replica_identity_t3;
