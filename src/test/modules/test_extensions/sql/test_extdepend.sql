--
-- test ALTER THING name DEPENDS ON EXTENSION
--

-- Common setup for all tests
CREATE TABLE test_extdep_commands (command text);
COPY test_extdep_commands FROM stdin;
 CREATE SCHEMA test_ext
 CREATE EXTENSION test_ext5 SCHEMA test_ext
 SET search_path TO test_ext
 CREATE TABLE a (a1 int)

 CREATE FUNCTION b() RETURNS TRIGGER LANGUAGE plpgsql AS\n   $$ BEGIN NEW.a1 := NEW.a1 + 42; RETURN NEW; END; $$
 ALTER FUNCTION b() DEPENDS ON EXTENSION test_ext5

 CREATE TRIGGER c BEFORE INSERT ON a FOR EACH ROW EXECUTE PROCEDURE b()
 ALTER TRIGGER c ON a DEPENDS ON EXTENSION test_ext5

 CREATE MATERIALIZED VIEW d AS SELECT * FROM a
 ALTER MATERIALIZED VIEW d DEPENDS ON EXTENSION test_ext5

 CREATE INDEX e ON a (a1)
 ALTER INDEX e DEPENDS ON EXTENSION test_ext5
 RESET search_path
\.

SELECT * FROM test_extdep_commands;
-- First, test that dependent objects go away when the extension is dropped.
SELECT * FROM test_extdep_commands \gexec
-- A dependent object made dependent again has no effect
ALTER FUNCTION test_ext.b() DEPENDS ON EXTENSION test_ext5;
-- make sure we have the right dependencies on the extension
SELECT deptype, p.*
  FROM pg_depend, pg_identify_object(classid, objid, objsubid) AS p
 WHERE refclassid = 'pg_extension'::regclass AND
       refobjid = (SELECT oid FROM pg_extension WHERE extname = 'test_ext5')
ORDER BY type;
DROP EXTENSION test_ext5;
-- anything still depending on the table?
SELECT deptype, i.*
  FROM pg_catalog.pg_depend, pg_identify_object(classid, objid, objsubid) i
WHERE refclassid='pg_class'::regclass AND
 refobjid='test_ext.a'::regclass AND NOT deptype IN ('i', 'a');
DROP SCHEMA test_ext CASCADE;

-- Second test: If we drop the table, the objects are dropped too and no
-- vestige remains in pg_depend.
SELECT * FROM test_extdep_commands \gexec
DROP TABLE test_ext.a;		-- should fail, require cascade
DROP TABLE test_ext.a CASCADE;
-- anything still depending on the extension?  Should be only function b()
SELECT deptype, i.*
  FROM pg_catalog.pg_depend, pg_identify_object(classid, objid, objsubid) i
 WHERE refclassid='pg_extension'::regclass AND
 refobjid=(SELECT oid FROM pg_extension WHERE extname='test_ext5');
DROP EXTENSION test_ext5;
DROP SCHEMA test_ext CASCADE;

-- Third test: we can drop the objects individually
SELECT * FROM test_extdep_commands \gexec
SET search_path TO test_ext;
DROP TRIGGER c ON a;
DROP FUNCTION b();
DROP MATERIALIZED VIEW d;
DROP INDEX e;

SELECT deptype, i.*
  FROM pg_catalog.pg_depend, pg_identify_object(classid, objid, objsubid) i
 WHERE (refclassid='pg_extension'::regclass AND
        refobjid=(SELECT oid FROM pg_extension WHERE extname='test_ext5'))
	OR (refclassid='pg_class'::regclass AND refobjid='test_ext.a'::regclass)
   AND NOT deptype IN ('i', 'a');
DROP TABLE a;
RESET search_path;
DROP SCHEMA test_ext CASCADE;

-- Fourth test: we can mark the objects as dependent, then unmark; then the
-- drop of the extension does nothing
SELECT * FROM test_extdep_commands \gexec
SET search_path TO test_ext;
ALTER FUNCTION b() NO DEPENDS ON EXTENSION test_ext5;
ALTER TRIGGER c ON a NO DEPENDS ON EXTENSION test_ext5;
ALTER MATERIALIZED VIEW d NO DEPENDS ON EXTENSION test_ext5;
ALTER INDEX e NO DEPENDS ON EXTENSION test_ext5;
DROP EXTENSION test_ext5;
DROP TRIGGER c ON a;
DROP FUNCTION b();
DROP MATERIALIZED VIEW d;
DROP INDEX e;
DROP SCHEMA test_ext CASCADE;

-- Fifth test: extension dependencies on partition indexes survive MERGE and
-- SPLIT PARTITION operations, and mismatches between source partitions are
-- reported.
RESET search_path;
CREATE EXTENSION test_ext3;
CREATE EXTENSION test_ext5;

CREATE TABLE part_extdep (i int, x int) PARTITION BY RANGE (i);
CREATE TABLE part_extdep_1 PARTITION OF part_extdep FOR VALUES FROM (1) TO (2);
CREATE TABLE part_extdep_2 PARTITION OF part_extdep FOR VALUES FROM (2) TO (3);
CREATE TABLE part_extdep_3 PARTITION OF part_extdep FOR VALUES FROM (3) TO (4);
CREATE TABLE part_extdep_4 PARTITION OF part_extdep FOR VALUES FROM (4) TO (5);
CREATE TABLE part_extdep_5 PARTITION OF part_extdep FOR VALUES FROM (5) TO (6);
CREATE INDEX part_extdep_i_idx ON part_extdep(i);
CREATE INDEX part_extdep_x_idx ON part_extdep(x);

-- Partitions 1, 2, 3 depend on the same two extensions.
ALTER INDEX part_extdep_1_i_idx DEPENDS ON EXTENSION test_ext3;
ALTER INDEX part_extdep_1_x_idx DEPENDS ON EXTENSION test_ext5;
ALTER INDEX part_extdep_2_i_idx DEPENDS ON EXTENSION test_ext3;
ALTER INDEX part_extdep_2_x_idx DEPENDS ON EXTENSION test_ext5;
ALTER INDEX part_extdep_3_i_idx DEPENDS ON EXTENSION test_ext3;
ALTER INDEX part_extdep_3_x_idx DEPENDS ON EXTENSION test_ext5;

-- Partition 4 depends on a different extension on one index.
ALTER INDEX part_extdep_4_i_idx DEPENDS ON EXTENSION test_ext5;

-- Partition 5 has no dependency at all.

-- Merge matching partitions: should succeed and preserve dependencies on the
-- new partition's indexes (DROP EXTENSION must fail, naming the new index).
ALTER TABLE part_extdep MERGE PARTITIONS (part_extdep_1, part_extdep_2)
    INTO part_extdep_merged;
DROP EXTENSION test_ext3;
SELECT c.relname, e.extname
FROM pg_depend d
JOIN pg_class c ON d.objid = c.oid
JOIN pg_extension e ON d.refobjid = e.oid
WHERE c.relname IN ('part_extdep_merged_i_idx', 'part_extdep_merged_x_idx')
  AND e.extname IN ('test_ext3', 'test_ext5')
  AND d.deptype = 'x'
ORDER BY c.relname, e.extname;

-- An index created directly on a partition has no parent in the partitioned
-- index tree.  Such an index is dropped with its old partition during merge,
-- and any extension dependency it carries goes away with it: the dep is not
-- promoted to the merged partition.  Verify by attaching test_ext9 to such
-- an orphan index, merging, and observing that test_ext9 becomes droppable.
CREATE EXTENSION test_ext9;
CREATE INDEX part_extdep_3_extra_idx ON part_extdep_3(x);
ALTER INDEX part_extdep_3_extra_idx DEPENDS ON EXTENSION test_ext9;
ALTER TABLE part_extdep MERGE PARTITIONS (part_extdep_merged, part_extdep_3)
    INTO part_extdep_merged2;
DROP EXTENSION test_ext9;

-- Mismatched dependencies: partition 4's index depends on a different
-- extension than partition_merged2's. Both orderings must fail, and the
-- error must cite both partition indexes.
ALTER TABLE part_extdep MERGE PARTITIONS (part_extdep_merged2, part_extdep_4)
    INTO part_extdep_bad;
ALTER TABLE part_extdep MERGE PARTITIONS (part_extdep_4, part_extdep_merged2)
    INTO part_extdep_bad;

-- Empty vs non-empty dependency set (the subset case the earlier linear
-- check missed in one direction).
ALTER TABLE part_extdep MERGE PARTITIONS (part_extdep_4, part_extdep_5)
    INTO part_extdep_bad;
ALTER TABLE part_extdep MERGE PARTITIONS (part_extdep_5, part_extdep_4)
    INTO part_extdep_bad;

-- Subset: partition 5's i_idx depends on a strict superset of partition 4's
-- i_idx dependencies. Partition 4 = {test_ext5}, partition 5 will be
-- {test_ext3, test_ext5}. Both orderings must fail; in particular the case
-- where the first partition we walk has fewer extensions than the second
-- must still be rejected.
ALTER INDEX part_extdep_5_i_idx DEPENDS ON EXTENSION test_ext3;
ALTER INDEX part_extdep_5_i_idx DEPENDS ON EXTENSION test_ext5;
ALTER TABLE part_extdep MERGE PARTITIONS (part_extdep_4, part_extdep_5)
    INTO part_extdep_bad;
ALTER TABLE part_extdep MERGE PARTITIONS (part_extdep_5, part_extdep_4)
    INTO part_extdep_bad;
-- Reset partition 5 so it doesn't interfere with the SPLIT test below.
ALTER INDEX part_extdep_5_i_idx NO DEPENDS ON EXTENSION test_ext3;
ALTER INDEX part_extdep_5_i_idx NO DEPENDS ON EXTENSION test_ext5;

-- Split: the single source partition's dependencies must appear on every
-- new partition's matching index, identified by extension name.
ALTER TABLE part_extdep SPLIT PARTITION part_extdep_merged2 INTO
    (PARTITION part_extdep_s1 FOR VALUES FROM (1) TO (3),
     PARTITION part_extdep_s2 FOR VALUES FROM (3) TO (4));
SELECT c.relname, e.extname
FROM pg_depend d
JOIN pg_class c ON d.objid = c.oid
JOIN pg_extension e ON d.refobjid = e.oid
WHERE c.relname IN ('part_extdep_s1_i_idx', 'part_extdep_s1_x_idx',
                    'part_extdep_s2_i_idx', 'part_extdep_s2_x_idx')
  AND e.extname IN ('test_ext3', 'test_ext5')
  AND d.deptype = 'x'
ORDER BY c.relname, e.extname;

DROP TABLE part_extdep;
DROP EXTENSION test_ext3;
DROP EXTENSION test_ext5;
