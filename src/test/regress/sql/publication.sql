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

SELECT pubname, puballtables FROM pg_publication WHERE pubname = 'testpub_foralltables';
\d+ testpub_tbl2
\dRp+ testpub_foralltables

DROP TABLE testpub_tbl2;
DROP PUBLICATION testpub_foralltables;

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
-- works despite missing REPLICA IDENTITY, because no actual update happened
UPDATE testpub_parted SET a = 1 WHERE false;
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
-- publication includes both the parent table and the child table
ALTER PUBLICATION testpub_forparted ADD TABLE testpub_parted, testpub_parted2;
-- only parent is listed as being in publication, not the partition
SELECT * FROM pg_publication_tables;
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

-- verify relation cache invalidation when a primary key is added using
-- an existing index
CREATE TABLE pub_test.testpub_addpk (id int not null, data int);
ALTER PUBLICATION testpub_default ADD TABLE pub_test.testpub_addpk;
INSERT INTO pub_test.testpub_addpk VALUES(1, 11);
CREATE UNIQUE INDEX testpub_addpk_id_idx ON pub_test.testpub_addpk(id);
-- fail:
UPDATE pub_test.testpub_addpk SET id = 2;
ALTER TABLE pub_test.testpub_addpk ADD PRIMARY KEY USING INDEX testpub_addpk_id_idx;
-- now it should work:
UPDATE pub_test.testpub_addpk SET id = 2;
DROP TABLE pub_test.testpub_addpk;

-- permissions
SET ROLE regress_publication_user2;
CREATE PUBLICATION testpub2;  -- fail

SET ROLE regress_publication_user;
GRANT CREATE ON DATABASE regression TO regress_publication_user2;
SET ROLE regress_publication_user2;
SET client_min_messages = 'ERROR';
CREATE PUBLICATION testpub2;  -- ok
RESET client_min_messages;

ALTER PUBLICATION testpub2 ADD TABLE testpub_tbl1;  -- fail

SET ROLE regress_publication_user;
GRANT regress_publication_user TO regress_publication_user2;
SET ROLE regress_publication_user2;
ALTER PUBLICATION testpub2 ADD TABLE testpub_tbl1;  -- ok

DROP PUBLICATION testpub2;

SET ROLE regress_publication_user;
REVOKE CREATE ON DATABASE regression FROM regress_publication_user2;

DROP TABLE testpub_parted;
DROP VIEW testpub_view;
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

DROP PUBLICATION testpub_default;
DROP PUBLICATION testpib_ins_trunct;
DROP PUBLICATION testpub_fortbl;

DROP SCHEMA pub_test CASCADE;

RESET SESSION AUTHORIZATION;
DROP ROLE regress_publication_user, regress_publication_user2;
DROP ROLE regress_publication_user_dummy;
