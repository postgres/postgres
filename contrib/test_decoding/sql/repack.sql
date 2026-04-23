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
