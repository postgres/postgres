-- Test REPACK (CONCURRENTLY).
-- This test isn't strictly about logical decoding per se, but
-- REPACK (CONCURRENTLY) involves logical decoding and therefore requires
-- to be run under higher than minimal wal_level, so we can't have them in
-- the main regression test suite.

-- Ownership of partitions is checked
CREATE TABLE ptnowner(i int unique not null) PARTITION BY LIST (i);
CREATE INDEX ptnowner_i_idx ON ptnowner(i);
CREATE TABLE ptnowner1 PARTITION OF ptnowner FOR VALUES IN (1);
CREATE ROLE regress_ptnowner;
CREATE TABLE ptnowner2 PARTITION OF ptnowner FOR VALUES IN (2);
ALTER TABLE ptnowner1 OWNER TO regress_ptnowner;
SET SESSION AUTHORIZATION regress_ptnowner;
ALTER TABLE ptnowner1 REPLICA IDENTITY USING INDEX ptnowner1_i_key;
REPACK (CONCURRENTLY) ptnowner1;
RESET SESSION AUTHORIZATION;
ALTER TABLE ptnowner OWNER TO regress_ptnowner;
CREATE TEMP TABLE ptnowner_oldnodes AS
  SELECT oid, relname, relfilenode FROM pg_partition_tree('ptnowner') AS tree
  JOIN pg_class AS c ON c.oid=tree.relid;
SELECT a.relname, a.relfilenode=b.relfilenode FROM pg_class a
  JOIN ptnowner_oldnodes b USING (oid) ORDER BY a.relname COLLATE "C";
DROP TABLE ptnowner;
DROP ROLE regress_ptnowner;

-- Verify that REPACK (CONCURRENTLY) doesn't lose "attmissingval" columns
CREATE TABLE rpk_missing (id int PRIMARY KEY);
INSERT INTO rpk_missing SELECT generate_series(1, 3);
ALTER TABLE rpk_missing ADD COLUMN a int DEFAULT 42;
SELECT * FROM rpk_missing;
REPACK (CONCURRENTLY) rpk_missing;
SELECT * FROM rpk_missing;
DROP TABLE rpk_missing;

-- Error cases for concurrent mode

-- Doesn't like partitioned tables
CREATE TABLE clstrpart (a int) PARTITION BY RANGE (a);
REPACK (CONCURRENTLY) clstrpart;

-- Disallowed in catalogs
REPACK (CONCURRENTLY) pg_class;

-- Doesn't support TOAST tables directly
CREATE TABLE repack_conc_toast (t text);
SELECT reltoastrelid::regclass AS toast_rel
FROM pg_class WHERE oid = 'repack_conc_toast'::regclass \gset
\set VERBOSITY sqlstate
REPACK (CONCURRENTLY) :toast_rel;
\set VERBOSITY default
DROP TABLE repack_conc_toast;

-- Only support permanent tables, temp and unlogged tables are not supported
CREATE TEMP TABLE repack_conc_temp (i int PRIMARY KEY);
REPACK (CONCURRENTLY) repack_conc_temp;
DROP TABLE repack_conc_temp;
CREATE UNLOGGED TABLE repack_conc_unlogged (i int PRIMARY KEY);
REPACK (CONCURRENTLY) repack_conc_unlogged;
DROP TABLE repack_conc_unlogged;

-- Doesn't support tables with REPLICA IDENTITY NOTHING, even if they have a primary key
CREATE TABLE repack_conc_replident (i int PRIMARY KEY);
ALTER TABLE repack_conc_replident REPLICA IDENTITY NOTHING;
REPACK (CONCURRENTLY) repack_conc_replident;

-- Doesn't support tables without a primary key or replica identity index
ALTER TABLE repack_conc_replident DROP CONSTRAINT repack_conc_replident_pkey;
ALTER TABLE repack_conc_replident REPLICA IDENTITY DEFAULT;
REPACK (CONCURRENTLY) repack_conc_replident;

-- Doesn't support tables with deferrable primary keys
ALTER TABLE repack_conc_replident ADD PRIMARY KEY (i) DEFERRABLE;
REPACK (CONCURRENTLY) repack_conc_replident;

-- clean up
DROP TABLE repack_conc_replident, clstrpart;

-- verify that the pgrepack plugin cannot be called directly
CREATE TABLE repack_plugin (a int);
SELECT * FROM pg_create_logical_replication_slot('s_repack', 'pgrepack');
INSERT INTO repack_plugin VALUES (1);
SELECT * FROM pg_logical_slot_get_binary_changes('s_repack', NULL, NULL);
SELECT pg_drop_replication_slot('s_repack');
