/* Test inheritance of structure (LIKE) */
CREATE TABLE inhx (xx text DEFAULT 'text');

/*
 * Test double inheritance
 *
 * Ensure that defaults are NOT included unless
 * INCLUDING DEFAULTS is specified
 */
CREATE TABLE ctla (aa TEXT);
CREATE TABLE ctlb (bb TEXT) INHERITS (ctla);

CREATE TABLE foo (LIKE nonexistent);

CREATE TABLE inhe (ee text, LIKE inhx) inherits (ctlb);
INSERT INTO inhe VALUES ('ee-col1', 'ee-col2', DEFAULT, 'ee-col4');
SELECT * FROM inhe; /* Columns aa, bb, xx value NULL, ee */
SELECT * FROM inhx; /* Empty set since LIKE inherits structure only */
SELECT * FROM ctlb; /* Has ee entry */
SELECT * FROM ctla; /* Has ee entry */

CREATE TABLE inhf (LIKE inhx, LIKE inhx); /* Throw error */

CREATE TABLE inhf (LIKE inhx INCLUDING DEFAULTS INCLUDING CONSTRAINTS);
INSERT INTO inhf DEFAULT VALUES;
SELECT * FROM inhf; /* Single entry with value 'text' */

ALTER TABLE inhx add constraint foo CHECK (xx = 'text');
ALTER TABLE inhx ADD PRIMARY KEY (xx);
CREATE TABLE inhg (LIKE inhx); /* Doesn't copy constraint */
INSERT INTO inhg VALUES ('foo');
DROP TABLE inhg;
CREATE TABLE inhg (x text, LIKE inhx INCLUDING CONSTRAINTS, y text); /* Copies constraints */
INSERT INTO inhg VALUES ('x', 'text', 'y'); /* Succeeds */
INSERT INTO inhg VALUES ('x', 'text', 'y'); /* Succeeds -- Unique constraints not copied */
INSERT INTO inhg VALUES ('x', 'foo',  'y');  /* fails due to constraint */
SELECT * FROM inhg; /* Two records with three columns in order x=x, xx=text, y=y */
DROP TABLE inhg;

CREATE TABLE inhg (x text, LIKE inhx INCLUDING INDEXES, y text); /* copies indexes */
INSERT INTO inhg VALUES (5, 10);
INSERT INTO inhg VALUES (20, 10); -- should fail
DROP TABLE inhg;
/* Multiple primary keys creation should fail */
CREATE TABLE inhg (x text, LIKE inhx INCLUDING INDEXES, PRIMARY KEY(x)); /* fails */
CREATE TABLE inhz (xx text DEFAULT 'text', yy int UNIQUE);
CREATE UNIQUE INDEX inhz_xx_idx on inhz (xx) WHERE xx <> 'test';
/* Ok to create multiple unique indexes */
CREATE TABLE inhg (x text UNIQUE, LIKE inhz INCLUDING INDEXES);
INSERT INTO inhg (xx, yy, x) VALUES ('test', 5, 10);
INSERT INTO inhg (xx, yy, x) VALUES ('test', 10, 15);
INSERT INTO inhg (xx, yy, x) VALUES ('foo', 10, 15); -- should fail
DROP TABLE inhg;
DROP TABLE inhz;

-- including storage and comments
CREATE TABLE ctlt1 (a text CHECK (length(a) > 2) PRIMARY KEY, b text);
CREATE INDEX ctlt1_b_key ON ctlt1 (b);
CREATE INDEX ctlt1_fnidx ON ctlt1 ((a || b));
COMMENT ON COLUMN ctlt1.a IS 'A';
COMMENT ON COLUMN ctlt1.b IS 'B';
COMMENT ON CONSTRAINT ctlt1_a_check ON ctlt1 IS 't1_a_check';
COMMENT ON INDEX ctlt1_pkey IS 'index pkey';
COMMENT ON INDEX ctlt1_b_key IS 'index b_key';
ALTER TABLE ctlt1 ALTER COLUMN a SET STORAGE MAIN;

CREATE TABLE ctlt2 (c text);
ALTER TABLE ctlt2 ALTER COLUMN c SET STORAGE EXTERNAL;
COMMENT ON COLUMN ctlt2.c IS 'C';

CREATE TABLE ctlt3 (a text CHECK (length(a) < 5), c text);
ALTER TABLE ctlt3 ALTER COLUMN c SET STORAGE EXTERNAL;
ALTER TABLE ctlt3 ALTER COLUMN a SET STORAGE MAIN;
COMMENT ON COLUMN ctlt3.a IS 'A3';
COMMENT ON COLUMN ctlt3.c IS 'C';
COMMENT ON CONSTRAINT ctlt3_a_check ON ctlt3 IS 't3_a_check';

CREATE TABLE ctlt4 (a text, c text);
ALTER TABLE ctlt4 ALTER COLUMN c SET STORAGE EXTERNAL;

CREATE TABLE ctlt12_storage (LIKE ctlt1 INCLUDING STORAGE, LIKE ctlt2 INCLUDING STORAGE);
\d+ ctlt12_storage
CREATE TABLE ctlt12_comments (LIKE ctlt1 INCLUDING COMMENTS, LIKE ctlt2 INCLUDING COMMENTS);
\d+ ctlt12_comments
CREATE TABLE ctlt1_inh (LIKE ctlt1 INCLUDING CONSTRAINTS INCLUDING COMMENTS) INHERITS (ctlt1);
\d+ ctlt1_inh
SELECT description FROM pg_description, pg_constraint c WHERE classoid = 'pg_constraint'::regclass AND objoid = c.oid AND c.conrelid = 'ctlt1_inh'::regclass;
CREATE TABLE ctlt13_inh () INHERITS (ctlt1, ctlt3);
\d+ ctlt13_inh
CREATE TABLE ctlt13_like (LIKE ctlt3 INCLUDING CONSTRAINTS INCLUDING COMMENTS INCLUDING STORAGE) INHERITS (ctlt1);
\d+ ctlt13_like
SELECT description FROM pg_description, pg_constraint c WHERE classoid = 'pg_constraint'::regclass AND objoid = c.oid AND c.conrelid = 'ctlt13_like'::regclass;

CREATE TABLE ctlt_all (LIKE ctlt1 INCLUDING ALL);
\d+ ctlt_all
SELECT c.relname, objsubid, description FROM pg_description, pg_index i, pg_class c WHERE classoid = 'pg_class'::regclass AND objoid = i.indexrelid AND c.oid = i.indexrelid AND i.indrelid = 'ctlt_all'::regclass ORDER BY c.relname, objsubid;

CREATE TABLE inh_error1 () INHERITS (ctlt1, ctlt4);
CREATE TABLE inh_error2 (LIKE ctlt4 INCLUDING STORAGE) INHERITS (ctlt1);

DROP TABLE ctlt1, ctlt2, ctlt3, ctlt4, ctlt12_storage, ctlt12_comments, ctlt1_inh, ctlt13_inh, ctlt13_like, ctlt_all, ctla, ctlb CASCADE;


/* LIKE with other relation kinds */

CREATE TABLE ctlt4 (a int, b text);

CREATE SEQUENCE ctlseq1;
CREATE TABLE ctlt10 (LIKE ctlseq1);  -- fail

CREATE VIEW ctlv1 AS SELECT * FROM ctlt4;
CREATE TABLE ctlt11 (LIKE ctlv1);
CREATE TABLE ctlt11a (LIKE ctlv1 INCLUDING ALL);

CREATE TYPE ctlty1 AS (a int, b text);
CREATE TABLE ctlt12 (LIKE ctlty1);

DROP SEQUENCE ctlseq1;
DROP TYPE ctlty1;
DROP VIEW ctlv1;
DROP TABLE IF EXISTS ctlt4, ctlt10, ctlt11, ctlt11a, ctlt12;

/* LIKE WITH OIDS */
CREATE TABLE has_oid (x INTEGER) WITH OIDS;
CREATE TABLE no_oid (y INTEGER);
CREATE TABLE like_test (z INTEGER, LIKE has_oid);
SELECT oid FROM like_test;
CREATE TABLE like_test2 (z INTEGER, LIKE no_oid);
SELECT oid FROM like_test2; -- fail
CREATE TABLE like_test3 (z INTEGER, LIKE has_oid, LIKE no_oid);
SELECT oid FROM like_test3;
CREATE TABLE like_test4 (z INTEGER, PRIMARY KEY(oid), LIKE has_oid);
SELECT oid FROM like_test4;
DROP TABLE has_oid, no_oid, like_test, like_test2, like_test3, like_test4;
