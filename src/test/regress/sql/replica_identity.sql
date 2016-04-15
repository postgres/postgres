CREATE TABLE test_replica_identity (
       id serial primary key,
       keya text not null,
       keyb text not null,
       nonkey text,
       CONSTRAINT test_replica_identity_unique_defer UNIQUE (keya, keyb) DEFERRABLE,
       CONSTRAINT test_replica_identity_unique_nondefer UNIQUE (keya, keyb)
) WITH OIDS;

CREATE TABLE test_replica_identity_othertable (id serial primary key);

CREATE INDEX test_replica_identity_keyab ON test_replica_identity (keya, keyb);
CREATE UNIQUE INDEX test_replica_identity_keyab_key ON test_replica_identity (keya, keyb);
CREATE UNIQUE INDEX test_replica_identity_oid_idx ON test_replica_identity (oid);
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

SELECT relreplident FROM pg_class WHERE oid = 'test_replica_identity'::regclass;

----
-- Make sure index cases succeed
----

-- succeed, primary key
ALTER TABLE test_replica_identity REPLICA IDENTITY USING INDEX test_replica_identity_pkey;
SELECT relreplident FROM pg_class WHERE oid = 'test_replica_identity'::regclass;
\d test_replica_identity

-- succeed, oid unique index
ALTER TABLE test_replica_identity REPLICA IDENTITY USING INDEX test_replica_identity_oid_idx;

-- succeed, nondeferrable unique constraint over nonullable cols
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

DROP TABLE test_replica_identity;
DROP TABLE test_replica_identity_othertable;
