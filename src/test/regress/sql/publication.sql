--
-- PUBLICATION
--
CREATE ROLE regress_publication_user LOGIN SUPERUSER;
CREATE ROLE regress_publication_user2;
CREATE ROLE regress_publication_user_dummy LOGIN NOSUPERUSER;
SET SESSION AUTHORIZATION 'regress_publication_user';

CREATE PUBLICATION testpub_default;

COMMENT ON PUBLICATION testpub_default IS 'test publication';
SELECT obj_description(p.oid, 'pg_publication') FROM pg_publication p;

CREATE PUBLICATION testpib_ins_trunct WITH (publish = insert);

ALTER PUBLICATION testpub_default SET (publish = update);

-- error cases
CREATE PUBLICATION testpub_xxx WITH (foo);
CREATE PUBLICATION testpub_xxx WITH (publish = 'cluster, vacuum');

\dRp

ALTER PUBLICATION testpub_default SET (publish = 'insert, update, delete');

\dRp

--- adding tables
CREATE SCHEMA pub_test;
CREATE TABLE testpub_tbl1 (id serial primary key, data text);
CREATE TABLE pub_test.testpub_nopk (foo int, bar int);
CREATE VIEW testpub_view AS SELECT 1;
CREATE TABLE testpub_parted (a int) PARTITION BY LIST (a);

CREATE PUBLICATION testpub_foralltables FOR ALL TABLES WITH (publish = 'insert');
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
CREATE PUBLICATION testpub3 FOR TABLE testpub_tbl3;
CREATE PUBLICATION testpub4 FOR TABLE ONLY testpub_tbl3;
\dRp+ testpub3
\dRp+ testpub4

DROP TABLE testpub_tbl3, testpub_tbl3a;
DROP PUBLICATION testpub3, testpub4;

-- fail - view
CREATE PUBLICATION testpub_fortbl FOR TABLE testpub_view;
CREATE PUBLICATION testpub_fortbl FOR TABLE testpub_tbl1, pub_test.testpub_nopk;
-- fail - already added
ALTER PUBLICATION testpub_fortbl ADD TABLE testpub_tbl1;
-- fail - already added
CREATE PUBLICATION testpub_fortbl FOR TABLE testpub_tbl1;

\dRp+ testpub_fortbl

-- fail - view
ALTER PUBLICATION testpub_default ADD TABLE testpub_view;
-- fail - partitioned table
ALTER PUBLICATION testpub_fortbl ADD TABLE testpub_parted;

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
CREATE PUBLICATION testpub2;  -- ok

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
