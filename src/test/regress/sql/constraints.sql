--
-- CONSTRAINTS
-- Constraints can be specified with:
--  - DEFAULT clause
--  - CHECK clauses
--  - PRIMARY KEY clauses
--  - UNIQUE clauses
--  - EXCLUDE clauses
--

-- directory paths are passed to us in environment variables
\getenv abs_srcdir PG_ABS_SRCDIR

--
-- DEFAULT syntax
--

CREATE TABLE DEFAULT_TBL (i int DEFAULT 100,
	x text DEFAULT 'vadim', f float8 DEFAULT 123.456);

INSERT INTO DEFAULT_TBL VALUES (1, 'thomas', 57.0613);
INSERT INTO DEFAULT_TBL VALUES (1, 'bruce');
INSERT INTO DEFAULT_TBL (i, f) VALUES (2, 987.654);
INSERT INTO DEFAULT_TBL (x) VALUES ('marc');
INSERT INTO DEFAULT_TBL VALUES (3, null, 1.0);

SELECT * FROM DEFAULT_TBL;

CREATE SEQUENCE DEFAULT_SEQ;

CREATE TABLE DEFAULTEXPR_TBL (i1 int DEFAULT 100 + (200-199) * 2,
	i2 int DEFAULT nextval('default_seq'));

INSERT INTO DEFAULTEXPR_TBL VALUES (-1, -2);
INSERT INTO DEFAULTEXPR_TBL (i1) VALUES (-3);
INSERT INTO DEFAULTEXPR_TBL (i2) VALUES (-4);
INSERT INTO DEFAULTEXPR_TBL (i2) VALUES (NULL);

SELECT * FROM DEFAULTEXPR_TBL;

-- syntax errors
--  test for extraneous comma
CREATE TABLE error_tbl (i int DEFAULT (100, ));
--  this will fail because gram.y uses b_expr not a_expr for defaults,
--  to avoid a shift/reduce conflict that arises from NOT NULL being
--  part of the column definition syntax:
CREATE TABLE error_tbl (b1 bool DEFAULT 1 IN (1, 2));
--  this should work, however:
CREATE TABLE error_tbl (b1 bool DEFAULT (1 IN (1, 2)));

DROP TABLE error_tbl;

--
-- CHECK syntax
--

CREATE TABLE CHECK_TBL (x int,
	CONSTRAINT CHECK_CON CHECK (x > 3));

INSERT INTO CHECK_TBL VALUES (5);
INSERT INTO CHECK_TBL VALUES (4);
INSERT INTO CHECK_TBL VALUES (3);
INSERT INTO CHECK_TBL VALUES (2);
INSERT INTO CHECK_TBL VALUES (6);
INSERT INTO CHECK_TBL VALUES (1);

SELECT * FROM CHECK_TBL;

CREATE SEQUENCE CHECK_SEQ;

CREATE TABLE CHECK2_TBL (x int, y text, z int,
	CONSTRAINT SEQUENCE_CON
	CHECK (x > 3 and y <> 'check failed' and z < 8));

INSERT INTO CHECK2_TBL VALUES (4, 'check ok', -2);
INSERT INTO CHECK2_TBL VALUES (1, 'x check failed', -2);
INSERT INTO CHECK2_TBL VALUES (5, 'z check failed', 10);
INSERT INTO CHECK2_TBL VALUES (0, 'check failed', -2);
INSERT INTO CHECK2_TBL VALUES (6, 'check failed', 11);
INSERT INTO CHECK2_TBL VALUES (7, 'check ok', 7);

SELECT * from CHECK2_TBL;

--
-- Check constraints on INSERT
--

CREATE SEQUENCE INSERT_SEQ;

CREATE TABLE INSERT_TBL (x INT DEFAULT nextval('insert_seq'),
	y TEXT DEFAULT '-NULL-',
	z INT DEFAULT -1 * currval('insert_seq'),
	CONSTRAINT INSERT_TBL_CON CHECK (x >= 3 AND y <> 'check failed' AND x < 8),
	CHECK (x + z = 0));

INSERT INTO INSERT_TBL(x,z) VALUES (2, -2);

SELECT * FROM INSERT_TBL;

SELECT 'one' AS one, nextval('insert_seq');

INSERT INTO INSERT_TBL(y) VALUES ('Y');
INSERT INTO INSERT_TBL(y) VALUES ('Y');
INSERT INTO INSERT_TBL(x,z) VALUES (1, -2);
INSERT INTO INSERT_TBL(z,x) VALUES (-7,  7);
INSERT INTO INSERT_TBL VALUES (5, 'check failed', -5);
INSERT INTO INSERT_TBL VALUES (7, '!check failed', -7);
INSERT INTO INSERT_TBL(y) VALUES ('-!NULL-');

SELECT * FROM INSERT_TBL;

INSERT INTO INSERT_TBL(y,z) VALUES ('check failed', 4);
INSERT INTO INSERT_TBL(x,y) VALUES (5, 'check failed');
INSERT INTO INSERT_TBL(x,y) VALUES (5, '!check failed');
INSERT INTO INSERT_TBL(y) VALUES ('-!NULL-');

SELECT * FROM INSERT_TBL;

SELECT 'seven' AS one, nextval('insert_seq');

INSERT INTO INSERT_TBL(y) VALUES ('Y');

SELECT 'eight' AS one, currval('insert_seq');

-- According to SQL, it is OK to insert a record that gives rise to NULL
-- constraint-condition results.  Postgres used to reject this, but it
-- was wrong:
INSERT INTO INSERT_TBL VALUES (null, null, null);

SELECT * FROM INSERT_TBL;

--
-- Check constraints on system columns
--

CREATE TABLE SYS_COL_CHECK_TBL (city text, state text, is_capital bool,
                  altitude int,
                  CHECK (NOT (is_capital AND tableoid::regclass::text = 'sys_col_check_tbl')));

INSERT INTO SYS_COL_CHECK_TBL VALUES ('Seattle', 'Washington', false, 100);
INSERT INTO SYS_COL_CHECK_TBL VALUES ('Olympia', 'Washington', true, 100);

SELECT *, tableoid::regclass::text FROM SYS_COL_CHECK_TBL;

DROP TABLE SYS_COL_CHECK_TBL;

--
-- Check constraints on system columns other then TableOid should return error
--
CREATE TABLE SYS_COL_CHECK_TBL (city text, state text, is_capital bool,
                  altitude int,
				  CHECK (NOT (is_capital AND ctid::text = 'sys_col_check_tbl')));

--
-- Check inheritance of defaults and constraints
--

CREATE TABLE INSERT_CHILD (cx INT default 42,
	cy INT CHECK (cy > x))
	INHERITS (INSERT_TBL);

INSERT INTO INSERT_CHILD(x,z,cy) VALUES (7,-7,11);
INSERT INTO INSERT_CHILD(x,z,cy) VALUES (7,-7,6);
INSERT INTO INSERT_CHILD(x,z,cy) VALUES (6,-7,7);
INSERT INTO INSERT_CHILD(x,y,z,cy) VALUES (6,'check failed',-6,7);

SELECT * FROM INSERT_CHILD;

DROP TABLE INSERT_CHILD;

--
-- Check NO INHERIT type of constraints and inheritance
--

CREATE TABLE ATACC1 (TEST INT
	CHECK (TEST > 0) NO INHERIT);

CREATE TABLE ATACC2 (TEST2 INT) INHERITS (ATACC1);
-- check constraint is not there on child
INSERT INTO ATACC2 (TEST) VALUES (-3);
-- check constraint is there on parent
INSERT INTO ATACC1 (TEST) VALUES (-3);
DROP TABLE ATACC1 CASCADE;

CREATE TABLE ATACC1 (TEST INT, TEST2 INT
	CHECK (TEST > 0), CHECK (TEST2 > 10) NO INHERIT);

CREATE TABLE ATACC2 () INHERITS (ATACC1);
-- check constraint is there on child
INSERT INTO ATACC2 (TEST) VALUES (-3);
-- check constraint is there on parent
INSERT INTO ATACC1 (TEST) VALUES (-3);
-- check constraint is not there on child
INSERT INTO ATACC2 (TEST2) VALUES (3);
-- check constraint is there on parent
INSERT INTO ATACC1 (TEST2) VALUES (3);
DROP TABLE ATACC1 CASCADE;

-- NOT NULL NO INHERIT
CREATE TABLE ATACC1 (a int, not null a no inherit);
CREATE TABLE ATACC2 () INHERITS (ATACC1);
\d+ ATACC2
DROP TABLE ATACC1, ATACC2;
CREATE TABLE ATACC1 (a int);
ALTER TABLE ATACC1 ADD NOT NULL a NO INHERIT;
CREATE TABLE ATACC2 () INHERITS (ATACC1);
\d+ ATACC2
DROP TABLE ATACC1, ATACC2;
CREATE TABLE ATACC1 (a int);
CREATE TABLE ATACC2 () INHERITS (ATACC1);
ALTER TABLE ATACC1 ADD NOT NULL a NO INHERIT;
\d+ ATACC2
DROP TABLE ATACC1, ATACC2;

-- overridding a no-inherit constraint with an inheritable one
CREATE TABLE ATACC2 (a int, CONSTRAINT a_is_not_null NOT NULL a NO INHERIT);
CREATE TABLE ATACC1 (a int);
CREATE TABLE ATACC3 (a int) INHERITS (ATACC2);
INSERT INTO ATACC3 VALUES (null);	-- make sure we scan atacc3
ALTER TABLE ATACC2 INHERIT ATACC1;
ALTER TABLE ATACC1 ADD CONSTRAINT ditto NOT NULL a;
DELETE FROM ATACC3;
ALTER TABLE ATACC1 ADD CONSTRAINT ditto NOT NULL a;
\d+ ATACC[123]
ALTER TABLE ATACC2 DROP CONSTRAINT a_is_not_null;
ALTER TABLE ATACC1 DROP CONSTRAINT ditto;
\d+ ATACC3
DROP TABLE ATACC1, ATACC2, ATACC3;

-- The same cannot be achieved this way
CREATE TABLE ATACC2 (a int, CONSTRAINT a_is_not_null NOT NULL a NO INHERIT);
CREATE TABLE ATACC1 (a int, CONSTRAINT ditto NOT NULL a);
CREATE TABLE ATACC3 (a int) INHERITS (ATACC2);
ALTER TABLE ATACC2 INHERIT ATACC1;
DROP TABLE ATACC1, ATACC2, ATACC3;

--
-- Check constraints on INSERT INTO
--

DELETE FROM INSERT_TBL;

ALTER SEQUENCE INSERT_SEQ RESTART WITH 4;

CREATE TEMP TABLE tmp (xd INT, yd TEXT, zd INT);

INSERT INTO tmp VALUES (null, 'Y', null);
INSERT INTO tmp VALUES (5, '!check failed', null);
INSERT INTO tmp VALUES (null, 'try again', null);
INSERT INTO INSERT_TBL(y) select yd from tmp;

SELECT * FROM INSERT_TBL;

INSERT INTO INSERT_TBL SELECT * FROM tmp WHERE yd = 'try again';
INSERT INTO INSERT_TBL(y,z) SELECT yd, -7 FROM tmp WHERE yd = 'try again';
INSERT INTO INSERT_TBL(y,z) SELECT yd, -8 FROM tmp WHERE yd = 'try again';

SELECT * FROM INSERT_TBL;

DROP TABLE tmp;

--
-- Check constraints on UPDATE
--

UPDATE INSERT_TBL SET x = NULL WHERE x = 5;
UPDATE INSERT_TBL SET x = 6 WHERE x = 6;
UPDATE INSERT_TBL SET x = -z, z = -x;
UPDATE INSERT_TBL SET x = z, z = x;

SELECT * FROM INSERT_TBL;

-- DROP TABLE INSERT_TBL;

--
-- Check constraints on COPY FROM
--

CREATE TABLE COPY_TBL (x INT, y TEXT, z INT,
	CONSTRAINT COPY_CON
	CHECK (x > 3 AND y <> 'check failed' AND x < 7 ));

\set filename :abs_srcdir '/data/constro.data'
COPY COPY_TBL FROM :'filename';

SELECT * FROM COPY_TBL;

\set filename :abs_srcdir '/data/constrf.data'
COPY COPY_TBL FROM :'filename';

SELECT * FROM COPY_TBL;

--
-- Primary keys
--

CREATE TABLE PRIMARY_TBL (i int PRIMARY KEY, t text);

INSERT INTO PRIMARY_TBL VALUES (1, 'one');
INSERT INTO PRIMARY_TBL VALUES (2, 'two');
INSERT INTO PRIMARY_TBL VALUES (1, 'three');
INSERT INTO PRIMARY_TBL VALUES (4, 'three');
INSERT INTO PRIMARY_TBL VALUES (5, 'one');
INSERT INTO PRIMARY_TBL (t) VALUES ('six');

SELECT * FROM PRIMARY_TBL;

DROP TABLE PRIMARY_TBL;

CREATE TABLE PRIMARY_TBL (i int, t text,
	PRIMARY KEY(i,t));

INSERT INTO PRIMARY_TBL VALUES (1, 'one');
INSERT INTO PRIMARY_TBL VALUES (2, 'two');
INSERT INTO PRIMARY_TBL VALUES (1, 'three');
INSERT INTO PRIMARY_TBL VALUES (4, 'three');
INSERT INTO PRIMARY_TBL VALUES (5, 'one');
INSERT INTO PRIMARY_TBL (t) VALUES ('six');

SELECT * FROM PRIMARY_TBL;

DROP TABLE PRIMARY_TBL;

--
-- Unique keys
--

CREATE TABLE UNIQUE_TBL (i int UNIQUE, t text);

INSERT INTO UNIQUE_TBL VALUES (1, 'one');
INSERT INTO UNIQUE_TBL VALUES (2, 'two');
INSERT INTO UNIQUE_TBL VALUES (1, 'three');
INSERT INTO UNIQUE_TBL VALUES (4, 'four');
INSERT INTO UNIQUE_TBL VALUES (5, 'one');
INSERT INTO UNIQUE_TBL (t) VALUES ('six');
INSERT INTO UNIQUE_TBL (t) VALUES ('seven');

INSERT INTO UNIQUE_TBL VALUES (5, 'five-upsert-insert') ON CONFLICT (i) DO UPDATE SET t = 'five-upsert-update';
INSERT INTO UNIQUE_TBL VALUES (6, 'six-upsert-insert') ON CONFLICT (i) DO UPDATE SET t = 'six-upsert-update';
-- should fail
INSERT INTO UNIQUE_TBL VALUES (1, 'a'), (2, 'b'), (2, 'b') ON CONFLICT (i) DO UPDATE SET t = 'fails';

SELECT * FROM UNIQUE_TBL;

DROP TABLE UNIQUE_TBL;

CREATE TABLE UNIQUE_TBL (i int UNIQUE NULLS NOT DISTINCT, t text);

INSERT INTO UNIQUE_TBL VALUES (1, 'one');
INSERT INTO UNIQUE_TBL VALUES (2, 'two');
INSERT INTO UNIQUE_TBL VALUES (1, 'three');  -- fail
INSERT INTO UNIQUE_TBL VALUES (4, 'four');
INSERT INTO UNIQUE_TBL VALUES (5, 'one');
INSERT INTO UNIQUE_TBL (t) VALUES ('six');
INSERT INTO UNIQUE_TBL (t) VALUES ('seven');  -- fail
INSERT INTO UNIQUE_TBL (t) VALUES ('eight') ON CONFLICT DO NOTHING;  -- no-op

SELECT * FROM UNIQUE_TBL;

DROP TABLE UNIQUE_TBL;

CREATE TABLE UNIQUE_TBL (i int, t text,
	UNIQUE(i,t));

INSERT INTO UNIQUE_TBL VALUES (1, 'one');
INSERT INTO UNIQUE_TBL VALUES (2, 'two');
INSERT INTO UNIQUE_TBL VALUES (1, 'three');
INSERT INTO UNIQUE_TBL VALUES (1, 'one');
INSERT INTO UNIQUE_TBL VALUES (5, 'one');
INSERT INTO UNIQUE_TBL (t) VALUES ('six');

SELECT * FROM UNIQUE_TBL;

DROP TABLE UNIQUE_TBL;

--
-- Deferrable unique constraints
--

CREATE TABLE unique_tbl (i int UNIQUE DEFERRABLE, t text);

INSERT INTO unique_tbl VALUES (0, 'one');
INSERT INTO unique_tbl VALUES (1, 'two');
INSERT INTO unique_tbl VALUES (2, 'tree');
INSERT INTO unique_tbl VALUES (3, 'four');
INSERT INTO unique_tbl VALUES (4, 'five');

BEGIN;

-- default is immediate so this should fail right away
UPDATE unique_tbl SET i = 1 WHERE i = 0;

ROLLBACK;

-- check is done at end of statement, so this should succeed
UPDATE unique_tbl SET i = i+1;

SELECT * FROM unique_tbl;

-- explicitly defer the constraint
BEGIN;

SET CONSTRAINTS unique_tbl_i_key DEFERRED;

INSERT INTO unique_tbl VALUES (3, 'three');
DELETE FROM unique_tbl WHERE t = 'tree'; -- makes constraint valid again

COMMIT; -- should succeed

SELECT * FROM unique_tbl;

-- try adding an initially deferred constraint
ALTER TABLE unique_tbl DROP CONSTRAINT unique_tbl_i_key;
ALTER TABLE unique_tbl ADD CONSTRAINT unique_tbl_i_key
	UNIQUE (i) DEFERRABLE INITIALLY DEFERRED;

BEGIN;

INSERT INTO unique_tbl VALUES (1, 'five');
INSERT INTO unique_tbl VALUES (5, 'one');
UPDATE unique_tbl SET i = 4 WHERE i = 2;
UPDATE unique_tbl SET i = 2 WHERE i = 4 AND t = 'four';
DELETE FROM unique_tbl WHERE i = 1 AND t = 'one';
DELETE FROM unique_tbl WHERE i = 5 AND t = 'five';

COMMIT;

SELECT * FROM unique_tbl;

-- should fail at commit-time
BEGIN;
INSERT INTO unique_tbl VALUES (3, 'Three'); -- should succeed for now
COMMIT; -- should fail

-- make constraint check immediate
BEGIN;

SET CONSTRAINTS ALL IMMEDIATE;

INSERT INTO unique_tbl VALUES (3, 'Three'); -- should fail

COMMIT;

-- forced check when SET CONSTRAINTS is called
BEGIN;

SET CONSTRAINTS ALL DEFERRED;

INSERT INTO unique_tbl VALUES (3, 'Three'); -- should succeed for now

SET CONSTRAINTS ALL IMMEDIATE; -- should fail

COMMIT;

-- test deferrable UNIQUE with a partitioned table
CREATE TABLE parted_uniq_tbl (i int UNIQUE DEFERRABLE) partition by range (i);
CREATE TABLE parted_uniq_tbl_1 PARTITION OF parted_uniq_tbl FOR VALUES FROM (0) TO (10);
CREATE TABLE parted_uniq_tbl_2 PARTITION OF parted_uniq_tbl FOR VALUES FROM (20) TO (30);
SELECT conname, conrelid::regclass FROM pg_constraint
  WHERE conname LIKE 'parted_uniq%' ORDER BY conname;
BEGIN;
INSERT INTO parted_uniq_tbl VALUES (1);
SAVEPOINT f;
INSERT INTO parted_uniq_tbl VALUES (1);	-- unique violation
ROLLBACK TO f;
SET CONSTRAINTS parted_uniq_tbl_i_key DEFERRED;
INSERT INTO parted_uniq_tbl VALUES (1);	-- OK now, fail at commit
COMMIT;
DROP TABLE parted_uniq_tbl;

-- test naming a constraint in a partition when a conflict exists
CREATE TABLE parted_fk_naming (
    id bigint NOT NULL default 1,
    id_abc bigint,
    CONSTRAINT dummy_constr FOREIGN KEY (id_abc)
        REFERENCES parted_fk_naming (id),
    PRIMARY KEY (id)
)
PARTITION BY LIST (id);
CREATE TABLE parted_fk_naming_1 (
    id bigint NOT NULL default 1,
    id_abc bigint,
    PRIMARY KEY (id),
    CONSTRAINT dummy_constr CHECK (true)
);
ALTER TABLE parted_fk_naming ATTACH PARTITION parted_fk_naming_1 FOR VALUES IN ('1');
SELECT conname FROM pg_constraint WHERE conrelid = 'parted_fk_naming_1'::regclass AND contype = 'f';
DROP TABLE parted_fk_naming;

-- test a HOT update that invalidates the conflicting tuple.
-- the trigger should still fire and catch the violation

BEGIN;

INSERT INTO unique_tbl VALUES (3, 'Three'); -- should succeed for now
UPDATE unique_tbl SET t = 'THREE' WHERE i = 3 AND t = 'Three';

COMMIT; -- should fail

SELECT * FROM unique_tbl;

-- test a HOT update that modifies the newly inserted tuple,
-- but should succeed because we then remove the other conflicting tuple.

BEGIN;

INSERT INTO unique_tbl VALUES(3, 'tree'); -- should succeed for now
UPDATE unique_tbl SET t = 'threex' WHERE t = 'tree';
DELETE FROM unique_tbl WHERE t = 'three';

SELECT * FROM unique_tbl;

COMMIT;

SELECT * FROM unique_tbl;

DROP TABLE unique_tbl;

--
-- EXCLUDE constraints
--

CREATE TABLE circles (
  c1 CIRCLE,
  c2 TEXT,
  EXCLUDE USING gist
    (c1 WITH &&, (c2::circle) WITH &&)
    WHERE (circle_center(c1) <> '(0,0)')
);

-- these should succeed because they don't match the index predicate
INSERT INTO circles VALUES('<(0,0), 5>', '<(0,0), 5>');
INSERT INTO circles VALUES('<(0,0), 5>', '<(0,0), 4>');

-- succeed
INSERT INTO circles VALUES('<(10,10), 10>', '<(0,0), 5>');
-- fail, overlaps
INSERT INTO circles VALUES('<(20,20), 10>', '<(0,0), 4>');
-- succeed, because violation is ignored
INSERT INTO circles VALUES('<(20,20), 10>', '<(0,0), 4>')
  ON CONFLICT ON CONSTRAINT circles_c1_c2_excl DO NOTHING;
-- fail, because DO UPDATE variant requires unique index
INSERT INTO circles VALUES('<(20,20), 10>', '<(0,0), 4>')
  ON CONFLICT ON CONSTRAINT circles_c1_c2_excl DO UPDATE SET c2 = EXCLUDED.c2;
-- succeed because c1 doesn't overlap
INSERT INTO circles VALUES('<(20,20), 1>', '<(0,0), 5>');
-- succeed because c2 doesn't overlap
INSERT INTO circles VALUES('<(20,20), 10>', '<(10,10), 5>');

-- should fail on existing data without the WHERE clause
ALTER TABLE circles ADD EXCLUDE USING gist
  (c1 WITH &&, (c2::circle) WITH &&);

-- try reindexing an existing constraint
REINDEX INDEX circles_c1_c2_excl;

DROP TABLE circles;

-- Check deferred exclusion constraint

CREATE TABLE deferred_excl (
  f1 int,
  f2 int,
  CONSTRAINT deferred_excl_con EXCLUDE (f1 WITH =) INITIALLY DEFERRED
);

INSERT INTO deferred_excl VALUES(1);
INSERT INTO deferred_excl VALUES(2);
INSERT INTO deferred_excl VALUES(1); -- fail
INSERT INTO deferred_excl VALUES(1) ON CONFLICT ON CONSTRAINT deferred_excl_con DO NOTHING; -- fail
BEGIN;
INSERT INTO deferred_excl VALUES(2); -- no fail here
COMMIT; -- should fail here
BEGIN;
INSERT INTO deferred_excl VALUES(3);
INSERT INTO deferred_excl VALUES(3); -- no fail here
COMMIT; -- should fail here

-- bug #13148: deferred constraint versus HOT update
BEGIN;
INSERT INTO deferred_excl VALUES(2, 1); -- no fail here
DELETE FROM deferred_excl WHERE f1 = 2 AND f2 IS NULL; -- remove old row
UPDATE deferred_excl SET f2 = 2 WHERE f1 = 2;
COMMIT; -- should not fail

SELECT * FROM deferred_excl;

ALTER TABLE deferred_excl DROP CONSTRAINT deferred_excl_con;

-- This should fail, but worth testing because of HOT updates
UPDATE deferred_excl SET f1 = 3;

ALTER TABLE deferred_excl ADD EXCLUDE (f1 WITH =);

DROP TABLE deferred_excl;

-- verify constraints created for NOT NULL clauses
CREATE TABLE notnull_tbl1 (a INTEGER NOT NULL NOT NULL);
\d+ notnull_tbl1
select conname, contype, conkey from pg_constraint where conrelid = 'notnull_tbl1'::regclass;
-- no-op
ALTER TABLE notnull_tbl1 ADD CONSTRAINT nn NOT NULL a;
\d+ notnull_tbl1
-- duplicate name
ALTER TABLE notnull_tbl1 ADD COLUMN b INT CONSTRAINT notnull_tbl1_a_not_null NOT NULL;
-- DROP NOT NULL gets rid of both the attnotnull flag and the constraint itself
ALTER TABLE notnull_tbl1 ALTER a DROP NOT NULL;
\d notnull_tbl1
select conname, contype, conkey from pg_constraint where conrelid = 'notnull_tbl1'::regclass;
-- SET NOT NULL puts both back
ALTER TABLE notnull_tbl1 ALTER a SET NOT NULL;
\d notnull_tbl1
select conname, contype, conkey from pg_constraint where conrelid = 'notnull_tbl1'::regclass;
-- Doing it twice doesn't create a redundant constraint
ALTER TABLE notnull_tbl1 ALTER a SET NOT NULL;
select conname, contype, conkey from pg_constraint where conrelid = 'notnull_tbl1'::regclass;
-- Using the "table constraint" syntax also works
ALTER TABLE notnull_tbl1 ALTER a DROP NOT NULL;
ALTER TABLE notnull_tbl1 ADD CONSTRAINT foobar NOT NULL a;
\d notnull_tbl1
select conname, contype, conkey from pg_constraint where conrelid = 'notnull_tbl1'::regclass;
DROP TABLE notnull_tbl1;

-- nope
CREATE TABLE notnull_tbl2 (a INTEGER CONSTRAINT blah NOT NULL, b INTEGER CONSTRAINT blah NOT NULL);

-- can't drop not-null in primary key
CREATE TABLE notnull_tbl2 (a INTEGER PRIMARY KEY);
ALTER TABLE notnull_tbl2 ALTER a DROP NOT NULL;
DROP TABLE notnull_tbl2;

-- make sure attnotnull is reset correctly when a PK is dropped indirectly,
-- or kept if there's a reason for that
CREATE TABLE notnull_tbl1 (c0 int, c1 int, PRIMARY KEY (c0, c1));
ALTER TABLE  notnull_tbl1 DROP c1;
\d+ notnull_tbl1
DROP TABLE notnull_tbl1;
-- same, via dropping a domain
CREATE DOMAIN notnull_dom1 AS INTEGER;
CREATE TABLE notnull_tbl1 (c0 notnull_dom1, c1 int, PRIMARY KEY (c0, c1));
DROP DOMAIN notnull_dom1 CASCADE;
\d+ notnull_tbl1
DROP TABLE notnull_tbl1;
-- with a REPLICA IDENTITY column.  Here the not-nulls must be kept
CREATE DOMAIN notnull_dom1 AS INTEGER;
CREATE TABLE notnull_tbl1 (c0 notnull_dom1, c1 int UNIQUE, c2 int generated by default as identity, PRIMARY KEY (c0, c1, c2));
ALTER TABLE notnull_tbl1 DROP CONSTRAINT notnull_tbl1_c2_not_null;
ALTER TABLE notnull_tbl1 REPLICA IDENTITY USING INDEX notnull_tbl1_c1_key;
DROP DOMAIN notnull_dom1 CASCADE;
ALTER TABLE  notnull_tbl1 ALTER c1 DROP NOT NULL;	-- can't be dropped
ALTER TABLE  notnull_tbl1 ALTER c1 SET NOT NULL;	-- can be set right
\d+ notnull_tbl1
DROP TABLE notnull_tbl1;

CREATE DOMAIN notnull_dom2 AS INTEGER;
CREATE TABLE notnull_tbl2 (c0 notnull_dom2, c1 int UNIQUE, c2 int generated by default as identity, PRIMARY KEY (c0, c1, c2));
ALTER TABLE notnull_tbl2 DROP CONSTRAINT notnull_tbl2_c2_not_null;
ALTER TABLE notnull_tbl2 REPLICA IDENTITY USING INDEX notnull_tbl2_c1_key;
DROP DOMAIN notnull_dom2 CASCADE;
\d+ notnull_tbl2
BEGIN;
/* make sure the table can be put right, but roll that back */
ALTER TABLE notnull_tbl2 REPLICA IDENTITY FULL, ALTER c2 DROP IDENTITY;
ALTER TABLE notnull_tbl2 ALTER c1 DROP NOT NULL, ALTER c2 DROP NOT NULL;
\d+ notnull_tbl2
ROLLBACK;
-- Leave this table around for pg_upgrade testing

CREATE TABLE notnull_tbl3 (a INTEGER NOT NULL, CHECK (a IS NOT NULL));
ALTER TABLE notnull_tbl3 ALTER A DROP NOT NULL;
ALTER TABLE notnull_tbl3 ADD b int, ADD CONSTRAINT pk PRIMARY KEY (a, b);
\d notnull_tbl3
ALTER TABLE notnull_tbl3 DROP CONSTRAINT pk;
\d notnull_tbl3

-- Primary keys in parent table cause NOT NULL constraint to spawn on their
-- children.  Verify that they work correctly.
CREATE TABLE cnn_parent (a int, b int);
CREATE TABLE cnn_child () INHERITS (cnn_parent);
CREATE TABLE cnn_grandchild (NOT NULL b) INHERITS (cnn_child);
CREATE TABLE cnn_child2 (NOT NULL a NO INHERIT) INHERITS (cnn_parent);
CREATE TABLE cnn_grandchild2 () INHERITS (cnn_grandchild, cnn_child2);

ALTER TABLE cnn_parent ADD PRIMARY KEY (b);
\d+ cnn_grandchild
\d+ cnn_grandchild2
ALTER TABLE cnn_parent DROP CONSTRAINT cnn_parent_pkey;
\set VERBOSITY terse
DROP TABLE cnn_parent CASCADE;
\set VERBOSITY default

-- As above, but create the primary key ahead of time
CREATE TABLE cnn_parent (a int, b int PRIMARY KEY);
CREATE TABLE cnn_child () INHERITS (cnn_parent);
CREATE TABLE cnn_grandchild (NOT NULL b) INHERITS (cnn_child);
CREATE TABLE cnn_child2 (NOT NULL a NO INHERIT) INHERITS (cnn_parent);
CREATE TABLE cnn_grandchild2 () INHERITS (cnn_grandchild, cnn_child2);

ALTER TABLE cnn_parent ADD PRIMARY KEY (b);
\d+ cnn_grandchild
\d+ cnn_grandchild2
ALTER TABLE cnn_parent DROP CONSTRAINT cnn_parent_pkey;
\set VERBOSITY terse
DROP TABLE cnn_parent CASCADE;
\set VERBOSITY default

-- As above, but create the primary key using a UNIQUE index
CREATE TABLE cnn_parent (a int, b int);
CREATE TABLE cnn_child () INHERITS (cnn_parent);
CREATE TABLE cnn_grandchild (NOT NULL b) INHERITS (cnn_child);
CREATE TABLE cnn_child2 (NOT NULL a NO INHERIT) INHERITS (cnn_parent);
CREATE TABLE cnn_grandchild2 () INHERITS (cnn_grandchild, cnn_child2);

CREATE UNIQUE INDEX b_uq ON cnn_parent (b);
ALTER TABLE cnn_parent ADD PRIMARY KEY USING INDEX b_uq;
\d+ cnn_grandchild
\d+ cnn_grandchild2
ALTER TABLE cnn_parent DROP CONSTRAINT cnn_parent_pkey;
-- keeps these tables around, for pg_upgrade testing

-- A primary key shouldn't attach to a unique constraint
create table cnn2_parted (a int primary key) partition by list (a);
create table cnn2_part1 (a int unique);
alter table cnn2_parted attach partition cnn2_part1 for values in (1);
\d+ cnn2_part1
drop table cnn2_parted;

-- ensure columns in partitions are marked not-null
create table cnn2_parted(a int primary key) partition by list (a);
create table cnn2_part1(a int);
alter table cnn2_parted attach partition cnn2_part1 for values in (1);
insert into cnn2_part1 values (null);
drop table cnn2_parted, cnn2_part1;

create table cnn2_parted(a int not null) partition by list (a);
create table cnn2_part1(a int primary key);
alter table cnn2_parted attach partition cnn2_part1 for values in (1);
drop table cnn2_parted, cnn2_part1;

create table cnn2_parted(a int) partition by list (a);
create table cnn_part1 partition of cnn2_parted for values in (1, null);
insert into cnn_part1 values (null);
alter table cnn2_parted add primary key (a);
drop table cnn2_parted;

-- columns in regular and LIKE inheritance should be marked not-nullable
-- for primary keys, even if those are deferred
CREATE TABLE notnull_tbl4 (a INTEGER PRIMARY KEY INITIALLY DEFERRED);
CREATE TABLE notnull_tbl4_lk (LIKE notnull_tbl4);
CREATE TABLE notnull_tbl4_lk2 (LIKE notnull_tbl4 INCLUDING INDEXES);
CREATE TABLE notnull_tbl4_lk3 (LIKE notnull_tbl4 INCLUDING INDEXES, CONSTRAINT a_nn NOT NULL a);
CREATE TABLE notnull_tbl4_cld () INHERITS (notnull_tbl4);
CREATE TABLE notnull_tbl4_cld2 (PRIMARY KEY (a) DEFERRABLE) INHERITS (notnull_tbl4);
CREATE TABLE notnull_tbl4_cld3 (PRIMARY KEY (a) DEFERRABLE, CONSTRAINT a_nn NOT NULL a) INHERITS (notnull_tbl4);
\d+ notnull_tbl4
\d+ notnull_tbl4_lk
\d+ notnull_tbl4_lk2
\d+ notnull_tbl4_lk3
\d+ notnull_tbl4_cld
\d+ notnull_tbl4_cld2
\d+ notnull_tbl4_cld3
-- leave these tables around for pg_upgrade testing

-- also, if a NOT NULL is dropped underneath a deferrable PK, the column
-- should still be nullable afterwards.  This mimics what pg_dump does.
CREATE TABLE notnull_tbl5 (a INTEGER CONSTRAINT a_nn NOT NULL);
ALTER TABLE notnull_tbl5 ADD PRIMARY KEY (a) DEFERRABLE;
ALTER TABLE notnull_tbl5 DROP CONSTRAINT a_nn;
\d+ notnull_tbl5
DROP TABLE notnull_tbl5;

-- Comments
-- Setup a low-level role to enforce non-superuser checks.
CREATE ROLE regress_constraint_comments;
SET SESSION AUTHORIZATION regress_constraint_comments;

CREATE TABLE constraint_comments_tbl (a int CONSTRAINT the_constraint CHECK (a > 0));
CREATE DOMAIN constraint_comments_dom AS int CONSTRAINT the_constraint CHECK (value > 0);

COMMENT ON CONSTRAINT the_constraint ON constraint_comments_tbl IS 'yes, the comment';
COMMENT ON CONSTRAINT the_constraint ON DOMAIN constraint_comments_dom IS 'yes, another comment';

-- no such constraint
COMMENT ON CONSTRAINT no_constraint ON constraint_comments_tbl IS 'yes, the comment';
COMMENT ON CONSTRAINT no_constraint ON DOMAIN constraint_comments_dom IS 'yes, another comment';

-- no such table/domain
COMMENT ON CONSTRAINT the_constraint ON no_comments_tbl IS 'bad comment';
COMMENT ON CONSTRAINT the_constraint ON DOMAIN no_comments_dom IS 'another bad comment';

COMMENT ON CONSTRAINT the_constraint ON constraint_comments_tbl IS NULL;
COMMENT ON CONSTRAINT the_constraint ON DOMAIN constraint_comments_dom IS NULL;

-- unauthorized user
RESET SESSION AUTHORIZATION;
CREATE ROLE regress_constraint_comments_noaccess;
SET SESSION AUTHORIZATION regress_constraint_comments_noaccess;
COMMENT ON CONSTRAINT the_constraint ON constraint_comments_tbl IS 'no, the comment';
COMMENT ON CONSTRAINT the_constraint ON DOMAIN constraint_comments_dom IS 'no, another comment';
RESET SESSION AUTHORIZATION;

DROP TABLE constraint_comments_tbl;
DROP DOMAIN constraint_comments_dom;

DROP ROLE regress_constraint_comments;
DROP ROLE regress_constraint_comments_noaccess;
