--
-- PUBLICATION
--
CREATE ROLE regress_publication_user LOGIN SUPERUSER;
CREATE ROLE regress_publication_user2;
CREATE ROLE regress_publication_user_dummy LOGIN NOSUPERUSER;
SET SESSION AUTHORIZATION 'regress_publication_user';

-- suppress warning that depends on wal_level
SET client_min_messages = 'ERROR';
CREATE PUBLICATION testpub_default;
RESET client_min_messages;

COMMENT ON PUBLICATION testpub_default IS 'test publication';
SELECT obj_description(p.oid, 'pg_publication') FROM pg_publication p;

SET client_min_messages = 'ERROR';
CREATE PUBLICATION testpib_ins_trunct WITH (publish = insert);
RESET client_min_messages;

ALTER PUBLICATION testpub_default SET (publish = update);

-- error cases
CREATE PUBLICATION testpub_xxx WITH (foo);
CREATE PUBLICATION testpub_xxx WITH (publish = 'cluster, vacuum');
CREATE PUBLICATION testpub_xxx WITH (publish_via_partition_root = 'true', publish_via_partition_root = '0');

\dRp

ALTER PUBLICATION testpub_default SET (publish = 'insert, update, delete');

\dRp

--- adding tables
CREATE SCHEMA pub_test;
CREATE TABLE testpub_tbl1 (id serial primary key, data text);
CREATE TABLE pub_test.testpub_nopk (foo int, bar int);
CREATE VIEW testpub_view AS SELECT 1;
CREATE TABLE testpub_parted (a int) PARTITION BY LIST (a);

SET client_min_messages = 'ERROR';
CREATE PUBLICATION testpub_foralltables FOR ALL TABLES WITH (publish = 'insert');
RESET client_min_messages;
ALTER PUBLICATION testpub_foralltables SET (publish = 'insert, update');

CREATE TABLE testpub_tbl2 (id serial primary key, data text);
-- fail - can't add to for all tables publication
ALTER PUBLICATION testpub_foralltables ADD TABLE testpub_tbl2;
-- fail - can't drop from all tables publication
ALTER PUBLICATION testpub_foralltables DROP TABLE testpub_tbl2;
-- fail - can't add to for all tables publication
ALTER PUBLICATION testpub_foralltables SET TABLE pub_test.testpub_nopk;

-- fail - can't add schema to 'FOR ALL TABLES' publication
ALTER PUBLICATION testpub_foralltables ADD ALL TABLES IN SCHEMA pub_test;
-- fail - can't drop schema from 'FOR ALL TABLES' publication
ALTER PUBLICATION testpub_foralltables DROP ALL TABLES IN SCHEMA pub_test;
-- fail - can't set schema to 'FOR ALL TABLES' publication
ALTER PUBLICATION testpub_foralltables SET ALL TABLES IN SCHEMA pub_test;

SET client_min_messages = 'ERROR';
CREATE PUBLICATION testpub_fortable FOR TABLE testpub_tbl1;
RESET client_min_messages;
-- should be able to add schema to 'FOR TABLE' publication
ALTER PUBLICATION testpub_fortable ADD ALL TABLES IN SCHEMA pub_test;
\dRp+ testpub_fortable
-- should be able to drop schema from 'FOR TABLE' publication
ALTER PUBLICATION testpub_fortable DROP ALL TABLES IN SCHEMA pub_test;
\dRp+ testpub_fortable
-- should be able to set schema to 'FOR TABLE' publication
ALTER PUBLICATION testpub_fortable SET ALL TABLES IN SCHEMA pub_test;
\dRp+ testpub_fortable

SET client_min_messages = 'ERROR';
CREATE PUBLICATION testpub_forschema FOR ALL TABLES IN SCHEMA pub_test;
RESET client_min_messages;
-- fail - can't create publication with schema and table of the same schema
CREATE PUBLICATION testpub_for_tbl_schema FOR ALL TABLES IN SCHEMA pub_test, TABLE pub_test.testpub_nopk;
-- fail - can't add a table of the same schema to the schema publication
ALTER PUBLICATION testpub_forschema ADD TABLE pub_test.testpub_nopk;
-- fail - can't drop a table from the schema publication which isn't in the
-- publication
ALTER PUBLICATION testpub_forschema DROP TABLE pub_test.testpub_nopk;
-- should be able to set table to schema publication
ALTER PUBLICATION testpub_forschema SET TABLE pub_test.testpub_nopk;
\dRp+ testpub_forschema

SELECT pubname, puballtables FROM pg_publication WHERE pubname = 'testpub_foralltables';
\d+ testpub_tbl2
\dRp+ testpub_foralltables

DROP TABLE testpub_tbl2;
DROP PUBLICATION testpub_foralltables, testpub_fortable, testpub_forschema;

CREATE TABLE testpub_tbl3 (a int);
CREATE TABLE testpub_tbl3a (b text) INHERITS (testpub_tbl3);
SET client_min_messages = 'ERROR';
CREATE PUBLICATION testpub3 FOR TABLE testpub_tbl3;
CREATE PUBLICATION testpub4 FOR TABLE ONLY testpub_tbl3;
RESET client_min_messages;
\dRp+ testpub3
\dRp+ testpub4

DROP TABLE testpub_tbl3, testpub_tbl3a;
DROP PUBLICATION testpub3, testpub4;

-- Tests for partitioned tables
SET client_min_messages = 'ERROR';
CREATE PUBLICATION testpub_forparted;
CREATE PUBLICATION testpub_forparted1;
RESET client_min_messages;
CREATE TABLE testpub_parted1 (LIKE testpub_parted);
CREATE TABLE testpub_parted2 (LIKE testpub_parted);
ALTER PUBLICATION testpub_forparted1 SET (publish='insert');
ALTER TABLE testpub_parted ATTACH PARTITION testpub_parted1 FOR VALUES IN (1);
ALTER TABLE testpub_parted ATTACH PARTITION testpub_parted2 FOR VALUES IN (2);
-- works despite missing REPLICA IDENTITY, because updates are not replicated
UPDATE testpub_parted1 SET a = 1;
-- only parent is listed as being in publication, not the partition
ALTER PUBLICATION testpub_forparted ADD TABLE testpub_parted;
\dRp+ testpub_forparted
-- should now fail, because parent's publication replicates updates
UPDATE testpub_parted1 SET a = 1;
ALTER TABLE testpub_parted DETACH PARTITION testpub_parted1;
-- works again, because parent's publication is no longer considered
UPDATE testpub_parted1 SET a = 1;
ALTER PUBLICATION testpub_forparted SET (publish_via_partition_root = true);
\dRp+ testpub_forparted
-- still fail, because parent's publication replicates updates
UPDATE testpub_parted2 SET a = 2;
ALTER PUBLICATION testpub_forparted DROP TABLE testpub_parted;
-- works again, because update is no longer replicated
UPDATE testpub_parted2 SET a = 2;
DROP TABLE testpub_parted1, testpub_parted2;
DROP PUBLICATION testpub_forparted, testpub_forparted1;

-- Test cache invalidation FOR ALL TABLES publication
SET client_min_messages = 'ERROR';
CREATE TABLE testpub_tbl4(a int);
INSERT INTO testpub_tbl4 values(1);
UPDATE testpub_tbl4 set a = 2;
CREATE PUBLICATION testpub_foralltables FOR ALL TABLES;
RESET client_min_messages;
-- fail missing REPLICA IDENTITY
UPDATE testpub_tbl4 set a = 3;
DROP PUBLICATION testpub_foralltables;
-- should pass after dropping the publication
UPDATE testpub_tbl4 set a = 3;
DROP TABLE testpub_tbl4;

-- fail - view
CREATE PUBLICATION testpub_fortbl FOR TABLE testpub_view;

CREATE TEMPORARY TABLE testpub_temptbl(a int);
-- fail - temporary table
CREATE PUBLICATION testpub_fortemptbl FOR TABLE testpub_temptbl;
DROP TABLE testpub_temptbl;

CREATE UNLOGGED TABLE testpub_unloggedtbl(a int);
-- fail - unlogged table
CREATE PUBLICATION testpub_forunloggedtbl FOR TABLE testpub_unloggedtbl;
DROP TABLE testpub_unloggedtbl;

-- fail - system table
CREATE PUBLICATION testpub_forsystemtbl FOR TABLE pg_publication;

SET client_min_messages = 'ERROR';
CREATE PUBLICATION testpub_fortbl FOR TABLE testpub_tbl1, pub_test.testpub_nopk;
RESET client_min_messages;
-- fail - already added
ALTER PUBLICATION testpub_fortbl ADD TABLE testpub_tbl1;
-- fail - already added
CREATE PUBLICATION testpub_fortbl FOR TABLE testpub_tbl1;

\dRp+ testpub_fortbl

-- fail - view
ALTER PUBLICATION testpub_default ADD TABLE testpub_view;

ALTER PUBLICATION testpub_default ADD TABLE testpub_tbl1;
ALTER PUBLICATION testpub_default SET TABLE testpub_tbl1;
ALTER PUBLICATION testpub_default ADD TABLE pub_test.testpub_nopk;

ALTER PUBLICATION testpib_ins_trunct ADD TABLE pub_test.testpub_nopk, testpub_tbl1;

\d+ pub_test.testpub_nopk
\d+ testpub_tbl1
\dRp+ testpub_default

ALTER PUBLICATION testpub_default DROP TABLE testpub_tbl1, pub_test.testpub_nopk;
-- fail - nonexistent
ALTER PUBLICATION testpub_default DROP TABLE pub_test.testpub_nopk;

\d+ testpub_tbl1

-- permissions
SET ROLE regress_publication_user2;
CREATE PUBLICATION testpub2;  -- fail

SET ROLE regress_publication_user;
GRANT CREATE ON DATABASE regression TO regress_publication_user2;
SET ROLE regress_publication_user2;
SET client_min_messages = 'ERROR';
CREATE PUBLICATION testpub2;  -- ok
CREATE PUBLICATION testpub3 FOR ALL TABLES IN SCHEMA pub_test;  -- fail
CREATE PUBLICATION testpub3;  -- ok
RESET client_min_messages;

ALTER PUBLICATION testpub2 ADD TABLE testpub_tbl1;  -- fail
ALTER PUBLICATION testpub3 ADD ALL TABLES IN SCHEMA pub_test;  -- fail

SET ROLE regress_publication_user;
GRANT regress_publication_user TO regress_publication_user2;
SET ROLE regress_publication_user2;
ALTER PUBLICATION testpub2 ADD TABLE testpub_tbl1;  -- ok

DROP PUBLICATION testpub2;
DROP PUBLICATION testpub3;

SET ROLE regress_publication_user;
CREATE ROLE regress_publication_user3;
GRANT regress_publication_user2 TO regress_publication_user3;
SET client_min_messages = 'ERROR';
CREATE PUBLICATION testpub4 FOR ALL TABLES IN SCHEMA pub_test;
RESET client_min_messages;
ALTER PUBLICATION testpub4 OWNER TO regress_publication_user3;
SET ROLE regress_publication_user3;
-- fail - new owner must be superuser
ALTER PUBLICATION testpub4 owner to regress_publication_user2; -- fail
ALTER PUBLICATION testpub4 owner to regress_publication_user; -- ok

SET ROLE regress_publication_user;
DROP PUBLICATION testpub4;
DROP ROLE regress_publication_user3;

REVOKE CREATE ON DATABASE regression FROM regress_publication_user2;

DROP TABLE testpub_parted;
DROP TABLE testpub_tbl1;

\dRp+ testpub_default

-- fail - must be owner of publication
SET ROLE regress_publication_user_dummy;
ALTER PUBLICATION testpub_default RENAME TO testpub_dummy;
RESET ROLE;

ALTER PUBLICATION testpub_default RENAME TO testpub_foo;

\dRp testpub_foo

-- rename back to keep the rest simple
ALTER PUBLICATION testpub_foo RENAME TO testpub_default;

ALTER PUBLICATION testpub_default OWNER TO regress_publication_user2;

\dRp testpub_default

-- adding schemas and tables
CREATE SCHEMA pub_test1;
CREATE SCHEMA pub_test2;
CREATE SCHEMA pub_test3;
CREATE SCHEMA "CURRENT_SCHEMA";
CREATE TABLE pub_test1.tbl (id int, data text);
CREATE TABLE pub_test1.tbl1 (id serial primary key, data text);
CREATE TABLE pub_test2.tbl1 (id serial primary key, data text);
CREATE TABLE "CURRENT_SCHEMA"."CURRENT_SCHEMA"(id int);

-- suppress warning that depends on wal_level
SET client_min_messages = 'ERROR';
CREATE PUBLICATION testpub1_forschema FOR ALL TABLES IN SCHEMA pub_test1;
\dRp+ testpub1_forschema

CREATE PUBLICATION testpub2_forschema FOR ALL TABLES IN SCHEMA pub_test1, pub_test2, pub_test3;
\dRp+ testpub2_forschema

-- check create publication on CURRENT_SCHEMA
CREATE PUBLICATION testpub3_forschema FOR ALL TABLES IN SCHEMA CURRENT_SCHEMA;
CREATE PUBLICATION testpub4_forschema FOR ALL TABLES IN SCHEMA "CURRENT_SCHEMA";
CREATE PUBLICATION testpub5_forschema FOR ALL TABLES IN SCHEMA CURRENT_SCHEMA, "CURRENT_SCHEMA";
CREATE PUBLICATION testpub6_forschema FOR ALL TABLES IN SCHEMA "CURRENT_SCHEMA", CURRENT_SCHEMA;
CREATE PUBLICATION testpub_fortable FOR TABLE "CURRENT_SCHEMA"."CURRENT_SCHEMA";

RESET client_min_messages;

\dRp+ testpub3_forschema
\dRp+ testpub4_forschema
\dRp+ testpub5_forschema
\dRp+ testpub6_forschema
\dRp+ testpub_fortable

-- check create publication on CURRENT_SCHEMA where search_path is not set
SET SEARCH_PATH='';
CREATE PUBLICATION testpub_forschema FOR ALL TABLES IN SCHEMA CURRENT_SCHEMA;
RESET SEARCH_PATH;

-- check create publication on CURRENT_SCHEMA where TABLE/ALL TABLES in SCHEMA
-- is not specified
CREATE PUBLICATION testpub_forschema1 FOR CURRENT_SCHEMA;

-- check create publication on CURRENT_SCHEMA along with FOR TABLE
CREATE PUBLICATION testpub_forschema1 FOR TABLE CURRENT_SCHEMA;

-- check create publication on a schema that does not exist
CREATE PUBLICATION testpub_forschema FOR ALL TABLES IN SCHEMA non_existent_schema;

-- check create publication on a system schema
CREATE PUBLICATION testpub_forschema FOR ALL TABLES IN SCHEMA pg_catalog;

-- check create publication on an object which is not schema
CREATE PUBLICATION testpub1_forschema1 FOR ALL TABLES IN SCHEMA testpub_view;

-- dropping the schema should reflect the change in publication
DROP SCHEMA pub_test3;
\dRp+ testpub2_forschema

-- renaming the schema should reflect the change in publication
ALTER SCHEMA pub_test1 RENAME to pub_test1_renamed;
\dRp+ testpub2_forschema

ALTER SCHEMA pub_test1_renamed RENAME to pub_test1;
\dRp+ testpub2_forschema

-- alter publication add schema
ALTER PUBLICATION testpub1_forschema ADD ALL TABLES IN SCHEMA pub_test2;
\dRp+ testpub1_forschema

-- add non existent schema
ALTER PUBLICATION testpub1_forschema ADD ALL TABLES IN SCHEMA non_existent_schema;
\dRp+ testpub1_forschema

-- add a schema which is already added to the publication
ALTER PUBLICATION testpub1_forschema ADD ALL TABLES IN SCHEMA pub_test1;
\dRp+ testpub1_forschema

-- alter publication drop schema
ALTER PUBLICATION testpub1_forschema DROP ALL TABLES IN SCHEMA pub_test2;
\dRp+ testpub1_forschema

-- drop schema that is not present in the publication
ALTER PUBLICATION testpub1_forschema DROP ALL TABLES IN SCHEMA pub_test2;
\dRp+ testpub1_forschema

-- drop a schema that does not exist in the system
ALTER PUBLICATION testpub1_forschema DROP ALL TABLES IN SCHEMA non_existent_schema;
\dRp+ testpub1_forschema

-- drop all schemas
ALTER PUBLICATION testpub1_forschema DROP ALL TABLES IN SCHEMA pub_test1;
\dRp+ testpub1_forschema

-- alter publication set multiple schema
ALTER PUBLICATION testpub1_forschema SET ALL TABLES IN SCHEMA pub_test1, pub_test2;
\dRp+ testpub1_forschema

-- alter publication set non-existent schema
ALTER PUBLICATION testpub1_forschema SET ALL TABLES IN SCHEMA non_existent_schema;
\dRp+ testpub1_forschema

-- alter publication set it duplicate schemas should set the schemas after
-- removing the duplicate schemas
ALTER PUBLICATION testpub1_forschema SET ALL TABLES IN SCHEMA pub_test1, pub_test1;
\dRp+ testpub1_forschema

-- cleanup pub_test1 schema for invalidation tests
ALTER PUBLICATION testpub2_forschema DROP ALL TABLES IN SCHEMA pub_test1;
DROP PUBLICATION testpub3_forschema, testpub4_forschema, testpub5_forschema, testpub6_forschema, testpub_fortable;
DROP SCHEMA "CURRENT_SCHEMA" CASCADE;

-- verify relation cache invalidations through update statement for the
-- default REPLICA IDENTITY on the relation, if schema is part of the
-- publication then update will fail because relation's relreplident
-- option will be set, if schema is not part of the publication then update
-- will be successful.
INSERT INTO pub_test1.tbl VALUES(1, 'test');

-- fail
UPDATE pub_test1.tbl SET id = 2;
ALTER PUBLICATION testpub1_forschema DROP ALL TABLES IN SCHEMA pub_test1;

-- success
UPDATE pub_test1.tbl SET id = 2;
ALTER PUBLICATION testpub1_forschema SET ALL TABLES IN SCHEMA pub_test1;

-- fail
UPDATE pub_test1.tbl SET id = 2;

-- verify invalidation of partition table having parent and child tables in
-- different schema
CREATE SCHEMA pub_testpart1;
CREATE SCHEMA pub_testpart2;

CREATE TABLE pub_testpart1.parent1 (a int) partition by list (a);
CREATE TABLE pub_testpart2.child_parent1 partition of pub_testpart1.parent1 for values in (1);
INSERT INTO pub_testpart2.child_parent1 values(1);
UPDATE pub_testpart2.child_parent1 set a = 1;
SET client_min_messages = 'ERROR';
CREATE PUBLICATION testpubpart_forschema FOR ALL TABLES IN SCHEMA pub_testpart1;
RESET client_min_messages;

-- fail
UPDATE pub_testpart1.parent1 set a = 1;
UPDATE pub_testpart2.child_parent1 set a = 1;

DROP PUBLICATION testpubpart_forschema;

-- verify invalidation of partition tables for schema publication that has
-- parent and child tables of different partition hierarchies
CREATE TABLE pub_testpart2.parent2 (a int) partition by list (a);
CREATE TABLE pub_testpart1.child_parent2 partition of pub_testpart2.parent2 for values in (1);
INSERT INTO pub_testpart1.child_parent2 values(1);
UPDATE pub_testpart1.child_parent2 set a = 1;
SET client_min_messages = 'ERROR';
CREATE PUBLICATION testpubpart_forschema FOR ALL TABLES IN SCHEMA pub_testpart2;
RESET client_min_messages;

-- fail
UPDATE pub_testpart2.child_parent1 set a = 1;
UPDATE pub_testpart2.parent2 set a = 1;
UPDATE pub_testpart1.child_parent2 set a = 1;

-- alter publication set 'ALL TABLES IN SCHEMA' on an empty publication.
SET client_min_messages = 'ERROR';
CREATE PUBLICATION testpub3_forschema;
RESET client_min_messages;
\dRp+ testpub3_forschema
ALTER PUBLICATION testpub3_forschema SET ALL TABLES IN SCHEMA pub_test1;
\dRp+ testpub3_forschema

-- create publication including both 'FOR TABLE' and 'FOR ALL TABLES IN SCHEMA'
SET client_min_messages = 'ERROR';
CREATE PUBLICATION testpub_forschema_fortable FOR ALL TABLES IN SCHEMA pub_test1, TABLE pub_test2.tbl1;
CREATE PUBLICATION testpub_fortable_forschema FOR TABLE pub_test2.tbl1, ALL TABLES IN SCHEMA pub_test1;
RESET client_min_messages;

\dRp+ testpub_forschema_fortable
\dRp+ testpub_fortable_forschema

-- fail specifying table without any of 'FOR ALL TABLES IN SCHEMA' or
--'FOR TABLE' or 'FOR ALL TABLES'
CREATE PUBLICATION testpub_error FOR pub_test2.tbl1;

DROP VIEW testpub_view;

DROP PUBLICATION testpub_default;
DROP PUBLICATION testpib_ins_trunct;
DROP PUBLICATION testpub_fortbl;
DROP PUBLICATION testpub1_forschema;
DROP PUBLICATION testpub2_forschema;
DROP PUBLICATION testpub3_forschema;
DROP PUBLICATION testpub_forschema_fortable;
DROP PUBLICATION testpub_fortable_forschema;
DROP PUBLICATION testpubpart_forschema;

DROP SCHEMA pub_test CASCADE;
DROP SCHEMA pub_test1 CASCADE;
DROP SCHEMA pub_test2 CASCADE;
DROP SCHEMA pub_testpart1 CASCADE;
DROP SCHEMA pub_testpart2 CASCADE;

-- Test the list of partitions published with or without
-- 'PUBLISH_VIA_PARTITION_ROOT' parameter
SET client_min_messages = 'ERROR';
CREATE SCHEMA sch1;
CREATE SCHEMA sch2;
CREATE TABLE sch1.tbl1 (a int) PARTITION BY RANGE(a);
CREATE TABLE sch2.tbl1_part1 PARTITION OF sch1.tbl1 FOR VALUES FROM (1) to (10);
-- Schema publication that does not include the schema that has the parent table
CREATE PUBLICATION pub FOR ALL TABLES IN SCHEMA sch2 WITH (PUBLISH_VIA_PARTITION_ROOT=1);
SELECT * FROM pg_publication_tables;

DROP PUBLICATION pub;
-- Table publication that does not include the parent table
CREATE PUBLICATION pub FOR TABLE sch2.tbl1_part1 WITH (PUBLISH_VIA_PARTITION_ROOT=1);
SELECT * FROM pg_publication_tables;

-- Table publication that includes both the parent table and the child table
ALTER PUBLICATION pub ADD TABLE sch1.tbl1;
SELECT * FROM pg_publication_tables;

DROP PUBLICATION pub;
-- Schema publication that does not include the schema that has the parent table
CREATE PUBLICATION pub FOR ALL TABLES IN SCHEMA sch2 WITH (PUBLISH_VIA_PARTITION_ROOT=0);
SELECT * FROM pg_publication_tables;

DROP PUBLICATION pub;
-- Table publication that does not include the parent table
CREATE PUBLICATION pub FOR TABLE sch2.tbl1_part1 WITH (PUBLISH_VIA_PARTITION_ROOT=0);
SELECT * FROM pg_publication_tables;

-- Table publication that includes both the parent table and the child table
ALTER PUBLICATION pub ADD TABLE sch1.tbl1;
SELECT * FROM pg_publication_tables;

DROP PUBLICATION pub;
DROP TABLE sch2.tbl1_part1;
DROP TABLE sch1.tbl1;

CREATE TABLE sch1.tbl1 (a int) PARTITION BY RANGE(a);
CREATE TABLE sch1.tbl1_part1 PARTITION OF sch1.tbl1 FOR VALUES FROM (1) to (10);
CREATE TABLE sch1.tbl1_part2 PARTITION OF sch1.tbl1 FOR VALUES FROM (10) to (20);
CREATE TABLE sch1.tbl1_part3 (a int) PARTITION BY RANGE(a);
ALTER TABLE sch1.tbl1 ATTACH PARTITION sch1.tbl1_part3 FOR VALUES FROM (20) to (30);
CREATE PUBLICATION pub FOR ALL TABLES IN SCHEMA sch1 WITH (PUBLISH_VIA_PARTITION_ROOT=1);
SELECT * FROM pg_publication_tables;

RESET client_min_messages;
DROP PUBLICATION pub;
DROP TABLE sch1.tbl1;
DROP SCHEMA sch1 cascade;
DROP SCHEMA sch2 cascade;

RESET SESSION AUTHORIZATION;
DROP ROLE regress_publication_user, regress_publication_user2;
DROP ROLE regress_publication_user_dummy;
