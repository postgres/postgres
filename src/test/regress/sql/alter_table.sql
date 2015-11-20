--
-- ALTER_TABLE
-- add attribute
--

CREATE TABLE tmp (initial int4);

COMMENT ON TABLE tmp_wrong IS 'table comment';
COMMENT ON TABLE tmp IS 'table comment';
COMMENT ON TABLE tmp IS NULL;

ALTER TABLE tmp ADD COLUMN xmin integer; -- fails

ALTER TABLE tmp ADD COLUMN a int4 default 3;

ALTER TABLE tmp ADD COLUMN b name;

ALTER TABLE tmp ADD COLUMN c text;

ALTER TABLE tmp ADD COLUMN d float8;

ALTER TABLE tmp ADD COLUMN e float4;

ALTER TABLE tmp ADD COLUMN f int2;

ALTER TABLE tmp ADD COLUMN g polygon;

ALTER TABLE tmp ADD COLUMN h abstime;

ALTER TABLE tmp ADD COLUMN i char;

ALTER TABLE tmp ADD COLUMN j abstime[];

ALTER TABLE tmp ADD COLUMN k int4;

ALTER TABLE tmp ADD COLUMN l tid;

ALTER TABLE tmp ADD COLUMN m xid;

ALTER TABLE tmp ADD COLUMN n oidvector;

--ALTER TABLE tmp ADD COLUMN o lock;
ALTER TABLE tmp ADD COLUMN p smgr;

ALTER TABLE tmp ADD COLUMN q point;

ALTER TABLE tmp ADD COLUMN r lseg;

ALTER TABLE tmp ADD COLUMN s path;

ALTER TABLE tmp ADD COLUMN t box;

ALTER TABLE tmp ADD COLUMN u tinterval;

ALTER TABLE tmp ADD COLUMN v timestamp;

ALTER TABLE tmp ADD COLUMN w interval;

ALTER TABLE tmp ADD COLUMN x float8[];

ALTER TABLE tmp ADD COLUMN y float4[];

ALTER TABLE tmp ADD COLUMN z int2[];

INSERT INTO tmp (a, b, c, d, e, f, g, h, i, j, k, l, m, n, p, q, r, s, t, u,
	v, w, x, y, z)
   VALUES (4, 'name', 'text', 4.1, 4.1, 2, '(4.1,4.1,3.1,3.1)',
        'Mon May  1 00:30:30 1995', 'c', '{Mon May  1 00:30:30 1995, Monday Aug 24 14:43:07 1992, epoch}',
	314159, '(1,1)', '512',
	'1 2 3 4 5 6 7 8', 'magnetic disk', '(1.1,1.1)', '(4.1,4.1,3.1,3.1)',
	'(0,2,4.1,4.1,3.1,3.1)', '(4.1,4.1,3.1,3.1)', '["epoch" "infinity"]',
	'epoch', '01:00:10', '{1.0,2.0,3.0,4.0}', '{1.0,2.0,3.0,4.0}', '{1,2,3,4}');

SELECT * FROM tmp;

DROP TABLE tmp;

-- the wolf bug - schema mods caused inconsistent row descriptors
CREATE TABLE tmp (
	initial 	int4
);

ALTER TABLE tmp ADD COLUMN a int4;

ALTER TABLE tmp ADD COLUMN b name;

ALTER TABLE tmp ADD COLUMN c text;

ALTER TABLE tmp ADD COLUMN d float8;

ALTER TABLE tmp ADD COLUMN e float4;

ALTER TABLE tmp ADD COLUMN f int2;

ALTER TABLE tmp ADD COLUMN g polygon;

ALTER TABLE tmp ADD COLUMN h abstime;

ALTER TABLE tmp ADD COLUMN i char;

ALTER TABLE tmp ADD COLUMN j abstime[];

ALTER TABLE tmp ADD COLUMN k int4;

ALTER TABLE tmp ADD COLUMN l tid;

ALTER TABLE tmp ADD COLUMN m xid;

ALTER TABLE tmp ADD COLUMN n oidvector;

--ALTER TABLE tmp ADD COLUMN o lock;
ALTER TABLE tmp ADD COLUMN p smgr;

ALTER TABLE tmp ADD COLUMN q point;

ALTER TABLE tmp ADD COLUMN r lseg;

ALTER TABLE tmp ADD COLUMN s path;

ALTER TABLE tmp ADD COLUMN t box;

ALTER TABLE tmp ADD COLUMN u tinterval;

ALTER TABLE tmp ADD COLUMN v timestamp;

ALTER TABLE tmp ADD COLUMN w interval;

ALTER TABLE tmp ADD COLUMN x float8[];

ALTER TABLE tmp ADD COLUMN y float4[];

ALTER TABLE tmp ADD COLUMN z int2[];

INSERT INTO tmp (a, b, c, d, e, f, g, h, i, j, k, l, m, n, p, q, r, s, t, u,
	v, w, x, y, z)
   VALUES (4, 'name', 'text', 4.1, 4.1, 2, '(4.1,4.1,3.1,3.1)',
        'Mon May  1 00:30:30 1995', 'c', '{Mon May  1 00:30:30 1995, Monday Aug 24 14:43:07 1992, epoch}',
	314159, '(1,1)', '512',
	'1 2 3 4 5 6 7 8', 'magnetic disk', '(1.1,1.1)', '(4.1,4.1,3.1,3.1)',
	'(0,2,4.1,4.1,3.1,3.1)', '(4.1,4.1,3.1,3.1)', '["epoch" "infinity"]',
	'epoch', '01:00:10', '{1.0,2.0,3.0,4.0}', '{1.0,2.0,3.0,4.0}', '{1,2,3,4}');

SELECT * FROM tmp;

DROP TABLE tmp;


--
-- rename - check on both non-temp and temp tables
--
CREATE TABLE tmp (regtable int);
CREATE TEMP TABLE tmp (tmptable int);

ALTER TABLE tmp RENAME TO tmp_new;

SELECT * FROM tmp;
SELECT * FROM tmp_new;

ALTER TABLE tmp RENAME TO tmp_new2;

SELECT * FROM tmp;		-- should fail
SELECT * FROM tmp_new;
SELECT * FROM tmp_new2;

DROP TABLE tmp_new;
DROP TABLE tmp_new2;


-- ALTER TABLE ... RENAME on non-table relations
-- renaming indexes (FIXME: this should probably test the index's functionality)
ALTER INDEX IF EXISTS __onek_unique1 RENAME TO tmp_onek_unique1;
ALTER INDEX IF EXISTS __tmp_onek_unique1 RENAME TO onek_unique1;

ALTER INDEX onek_unique1 RENAME TO tmp_onek_unique1;
ALTER INDEX tmp_onek_unique1 RENAME TO onek_unique1;
-- renaming views
CREATE VIEW tmp_view (unique1) AS SELECT unique1 FROM tenk1;
ALTER TABLE tmp_view RENAME TO tmp_view_new;

-- hack to ensure we get an indexscan here
set enable_seqscan to off;
set enable_bitmapscan to off;
-- 5 values, sorted
SELECT unique1 FROM tenk1 WHERE unique1 < 5;
reset enable_seqscan;
reset enable_bitmapscan;

DROP VIEW tmp_view_new;
-- toast-like relation name
alter table stud_emp rename to pg_toast_stud_emp;
alter table pg_toast_stud_emp rename to stud_emp;

-- renaming index should rename constraint as well
ALTER TABLE onek ADD CONSTRAINT onek_unique1_constraint UNIQUE (unique1);
ALTER INDEX onek_unique1_constraint RENAME TO onek_unique1_constraint_foo;
ALTER TABLE onek DROP CONSTRAINT onek_unique1_constraint_foo;

-- renaming constraint
ALTER TABLE onek ADD CONSTRAINT onek_check_constraint CHECK (unique1 >= 0);
ALTER TABLE onek RENAME CONSTRAINT onek_check_constraint TO onek_check_constraint_foo;
ALTER TABLE onek DROP CONSTRAINT onek_check_constraint_foo;

-- renaming constraint should rename index as well
ALTER TABLE onek ADD CONSTRAINT onek_unique1_constraint UNIQUE (unique1);
DROP INDEX onek_unique1_constraint;  -- to see whether it's there
ALTER TABLE onek RENAME CONSTRAINT onek_unique1_constraint TO onek_unique1_constraint_foo;
DROP INDEX onek_unique1_constraint_foo;  -- to see whether it's there
ALTER TABLE onek DROP CONSTRAINT onek_unique1_constraint_foo;

-- renaming constraints vs. inheritance
CREATE TABLE constraint_rename_test (a int CONSTRAINT con1 CHECK (a > 0), b int, c int);
\d constraint_rename_test
CREATE TABLE constraint_rename_test2 (a int CONSTRAINT con1 CHECK (a > 0), d int) INHERITS (constraint_rename_test);
\d constraint_rename_test2
ALTER TABLE constraint_rename_test2 RENAME CONSTRAINT con1 TO con1foo; -- fail
ALTER TABLE ONLY constraint_rename_test RENAME CONSTRAINT con1 TO con1foo; -- fail
ALTER TABLE constraint_rename_test RENAME CONSTRAINT con1 TO con1foo; -- ok
\d constraint_rename_test
\d constraint_rename_test2
ALTER TABLE constraint_rename_test ADD CONSTRAINT con2 CHECK (b > 0) NO INHERIT;
ALTER TABLE ONLY constraint_rename_test RENAME CONSTRAINT con2 TO con2foo; -- ok
ALTER TABLE constraint_rename_test RENAME CONSTRAINT con2foo TO con2bar; -- ok
\d constraint_rename_test
\d constraint_rename_test2
ALTER TABLE constraint_rename_test ADD CONSTRAINT con3 PRIMARY KEY (a);
ALTER TABLE constraint_rename_test RENAME CONSTRAINT con3 TO con3foo; -- ok
\d constraint_rename_test
\d constraint_rename_test2
DROP TABLE constraint_rename_test2;
DROP TABLE constraint_rename_test;
ALTER TABLE IF EXISTS constraint_not_exist RENAME CONSTRAINT con3 TO con3foo; -- ok
ALTER TABLE IF EXISTS constraint_rename_test ADD CONSTRAINT con4 UNIQUE (a);

-- FOREIGN KEY CONSTRAINT adding TEST

CREATE TABLE tmp2 (a int primary key);

CREATE TABLE tmp3 (a int, b int);

CREATE TABLE tmp4 (a int, b int, unique(a,b));

CREATE TABLE tmp5 (a int, b int);

-- Insert rows into tmp2 (pktable)
INSERT INTO tmp2 values (1);
INSERT INTO tmp2 values (2);
INSERT INTO tmp2 values (3);
INSERT INTO tmp2 values (4);

-- Insert rows into tmp3
INSERT INTO tmp3 values (1,10);
INSERT INTO tmp3 values (1,20);
INSERT INTO tmp3 values (5,50);

-- Try (and fail) to add constraint due to invalid source columns
ALTER TABLE tmp3 add constraint tmpconstr foreign key(c) references tmp2 match full;

-- Try (and fail) to add constraint due to invalide destination columns explicitly given
ALTER TABLE tmp3 add constraint tmpconstr foreign key(a) references tmp2(b) match full;

-- Try (and fail) to add constraint due to invalid data
ALTER TABLE tmp3 add constraint tmpconstr foreign key (a) references tmp2 match full;

-- Delete failing row
DELETE FROM tmp3 where a=5;

-- Try (and succeed)
ALTER TABLE tmp3 add constraint tmpconstr foreign key (a) references tmp2 match full;
ALTER TABLE tmp3 drop constraint tmpconstr;

INSERT INTO tmp3 values (5,50);

-- Try NOT VALID and then VALIDATE CONSTRAINT, but fails. Delete failure then re-validate
ALTER TABLE tmp3 add constraint tmpconstr foreign key (a) references tmp2 match full NOT VALID;
ALTER TABLE tmp3 validate constraint tmpconstr;

-- Delete failing row
DELETE FROM tmp3 where a=5;

-- Try (and succeed) and repeat to show it works on already valid constraint
ALTER TABLE tmp3 validate constraint tmpconstr;
ALTER TABLE tmp3 validate constraint tmpconstr;

-- Try a non-verified CHECK constraint
ALTER TABLE tmp3 ADD CONSTRAINT b_greater_than_ten CHECK (b > 10); -- fail
ALTER TABLE tmp3 ADD CONSTRAINT b_greater_than_ten CHECK (b > 10) NOT VALID; -- succeeds
ALTER TABLE tmp3 VALIDATE CONSTRAINT b_greater_than_ten; -- fails
DELETE FROM tmp3 WHERE NOT b > 10;
ALTER TABLE tmp3 VALIDATE CONSTRAINT b_greater_than_ten; -- succeeds
ALTER TABLE tmp3 VALIDATE CONSTRAINT b_greater_than_ten; -- succeeds

-- Test inherited NOT VALID CHECK constraints
select * from tmp3;
CREATE TABLE tmp6 () INHERITS (tmp3);
CREATE TABLE tmp7 () INHERITS (tmp3);

INSERT INTO tmp6 VALUES (6, 30), (7, 16);
ALTER TABLE tmp3 ADD CONSTRAINT b_le_20 CHECK (b <= 20) NOT VALID;
ALTER TABLE tmp3 VALIDATE CONSTRAINT b_le_20;	-- fails
DELETE FROM tmp6 WHERE b > 20;
ALTER TABLE tmp3 VALIDATE CONSTRAINT b_le_20;	-- succeeds

-- An already validated constraint must not be revalidated
CREATE FUNCTION boo(int) RETURNS int IMMUTABLE STRICT LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'boo: %', $1; RETURN $1; END; $$;
INSERT INTO tmp7 VALUES (8, 18);
ALTER TABLE tmp7 ADD CONSTRAINT identity CHECK (b = boo(b));
ALTER TABLE tmp3 ADD CONSTRAINT IDENTITY check (b = boo(b)) NOT VALID;
ALTER TABLE tmp3 VALIDATE CONSTRAINT identity;

-- Try (and fail) to create constraint from tmp5(a) to tmp4(a) - unique constraint on
-- tmp4 is a,b

ALTER TABLE tmp5 add constraint tmpconstr foreign key(a) references tmp4(a) match full;

DROP TABLE tmp7;

DROP TABLE tmp6;

DROP TABLE tmp5;

DROP TABLE tmp4;

DROP TABLE tmp3;

DROP TABLE tmp2;

-- NOT VALID with plan invalidation -- ensure we don't use a constraint for
-- exclusion until validated
set constraint_exclusion TO 'partition';
create table nv_parent (d date);
create table nv_child_2010 () inherits (nv_parent);
create table nv_child_2011 () inherits (nv_parent);
alter table nv_child_2010 add check (d between '2010-01-01'::date and '2010-12-31'::date) not valid;
alter table nv_child_2011 add check (d between '2011-01-01'::date and '2011-12-31'::date) not valid;
explain (costs off) select * from nv_parent where d between '2011-08-01' and '2011-08-31';
create table nv_child_2009 (check (d between '2009-01-01'::date and '2009-12-31'::date)) inherits (nv_parent);
explain (costs off) select * from nv_parent where d between '2011-08-01'::date and '2011-08-31'::date;
explain (costs off) select * from nv_parent where d between '2009-08-01'::date and '2009-08-31'::date;
-- after validation, the constraint should be used
alter table nv_child_2011 VALIDATE CONSTRAINT nv_child_2011_d_check;
explain (costs off) select * from nv_parent where d between '2009-08-01'::date and '2009-08-31'::date;

-- add an inherited NOT VALID constraint
alter table nv_parent add check (d between '2001-01-01'::date and '2099-12-31'::date) not valid;
\d nv_child_2009
-- we leave nv_parent and children around to help test pg_dump logic

-- Foreign key adding test with mixed types

-- Note: these tables are TEMP to avoid name conflicts when this test
-- is run in parallel with foreign_key.sql.

CREATE TEMP TABLE PKTABLE (ptest1 int PRIMARY KEY);
INSERT INTO PKTABLE VALUES(42);
CREATE TEMP TABLE FKTABLE (ftest1 inet);
-- This next should fail, because int=inet does not exist
ALTER TABLE FKTABLE ADD FOREIGN KEY(ftest1) references pktable;
-- This should also fail for the same reason, but here we
-- give the column name
ALTER TABLE FKTABLE ADD FOREIGN KEY(ftest1) references pktable(ptest1);
DROP TABLE FKTABLE;
-- This should succeed, even though they are different types,
-- because int=int8 exists and is a member of the integer opfamily
CREATE TEMP TABLE FKTABLE (ftest1 int8);
ALTER TABLE FKTABLE ADD FOREIGN KEY(ftest1) references pktable;
-- Check it actually works
INSERT INTO FKTABLE VALUES(42);		-- should succeed
INSERT INTO FKTABLE VALUES(43);		-- should fail
DROP TABLE FKTABLE;
-- This should fail, because we'd have to cast numeric to int which is
-- not an implicit coercion (or use numeric=numeric, but that's not part
-- of the integer opfamily)
CREATE TEMP TABLE FKTABLE (ftest1 numeric);
ALTER TABLE FKTABLE ADD FOREIGN KEY(ftest1) references pktable;
DROP TABLE FKTABLE;
DROP TABLE PKTABLE;
-- On the other hand, this should work because int implicitly promotes to
-- numeric, and we allow promotion on the FK side
CREATE TEMP TABLE PKTABLE (ptest1 numeric PRIMARY KEY);
INSERT INTO PKTABLE VALUES(42);
CREATE TEMP TABLE FKTABLE (ftest1 int);
ALTER TABLE FKTABLE ADD FOREIGN KEY(ftest1) references pktable;
-- Check it actually works
INSERT INTO FKTABLE VALUES(42);		-- should succeed
INSERT INTO FKTABLE VALUES(43);		-- should fail
DROP TABLE FKTABLE;
DROP TABLE PKTABLE;

CREATE TEMP TABLE PKTABLE (ptest1 int, ptest2 inet,
                           PRIMARY KEY(ptest1, ptest2));
-- This should fail, because we just chose really odd types
CREATE TEMP TABLE FKTABLE (ftest1 cidr, ftest2 timestamp);
ALTER TABLE FKTABLE ADD FOREIGN KEY(ftest1, ftest2) references pktable;
DROP TABLE FKTABLE;
-- Again, so should this...
CREATE TEMP TABLE FKTABLE (ftest1 cidr, ftest2 timestamp);
ALTER TABLE FKTABLE ADD FOREIGN KEY(ftest1, ftest2)
     references pktable(ptest1, ptest2);
DROP TABLE FKTABLE;
-- This fails because we mixed up the column ordering
CREATE TEMP TABLE FKTABLE (ftest1 int, ftest2 inet);
ALTER TABLE FKTABLE ADD FOREIGN KEY(ftest1, ftest2)
     references pktable(ptest2, ptest1);
-- As does this...
ALTER TABLE FKTABLE ADD FOREIGN KEY(ftest2, ftest1)
     references pktable(ptest1, ptest2);

-- temp tables should go away by themselves, need not drop them.

-- test check constraint adding

create table atacc1 ( test int );
-- add a check constraint
alter table atacc1 add constraint atacc_test1 check (test>3);
-- should fail
insert into atacc1 (test) values (2);
-- should succeed
insert into atacc1 (test) values (4);
drop table atacc1;

-- let's do one where the check fails when added
create table atacc1 ( test int );
-- insert a soon to be failing row
insert into atacc1 (test) values (2);
-- add a check constraint (fails)
alter table atacc1 add constraint atacc_test1 check (test>3);
insert into atacc1 (test) values (4);
drop table atacc1;

-- let's do one where the check fails because the column doesn't exist
create table atacc1 ( test int );
-- add a check constraint (fails)
alter table atacc1 add constraint atacc_test1 check (test1>3);
drop table atacc1;

-- something a little more complicated
create table atacc1 ( test int, test2 int, test3 int);
-- add a check constraint (fails)
alter table atacc1 add constraint atacc_test1 check (test+test2<test3*4);
-- should fail
insert into atacc1 (test,test2,test3) values (4,4,2);
-- should succeed
insert into atacc1 (test,test2,test3) values (4,4,5);
drop table atacc1;

-- lets do some naming tests
create table atacc1 (test int check (test>3), test2 int);
alter table atacc1 add check (test2>test);
-- should fail for $2
insert into atacc1 (test2, test) values (3, 4);
drop table atacc1;

-- inheritance related tests
create table atacc1 (test int);
create table atacc2 (test2 int);
create table atacc3 (test3 int) inherits (atacc1, atacc2);
alter table atacc2 add constraint foo check (test2>0);
-- fail and then succeed on atacc2
insert into atacc2 (test2) values (-3);
insert into atacc2 (test2) values (3);
-- fail and then succeed on atacc3
insert into atacc3 (test2) values (-3);
insert into atacc3 (test2) values (3);
drop table atacc3;
drop table atacc2;
drop table atacc1;

-- same things with one created with INHERIT
create table atacc1 (test int);
create table atacc2 (test2 int);
create table atacc3 (test3 int) inherits (atacc1, atacc2);
alter table atacc3 no inherit atacc2;
-- fail
alter table atacc3 no inherit atacc2;
-- make sure it really isn't a child
insert into atacc3 (test2) values (3);
select test2 from atacc2;
-- fail due to missing constraint
alter table atacc2 add constraint foo check (test2>0);
alter table atacc3 inherit atacc2;
-- fail due to missing column
alter table atacc3 rename test2 to testx;
alter table atacc3 inherit atacc2;
-- fail due to mismatched data type
alter table atacc3 add test2 bool;
alter table atacc3 inherit atacc2;
alter table atacc3 drop test2;
-- succeed
alter table atacc3 add test2 int;
update atacc3 set test2 = 4 where test2 is null;
alter table atacc3 add constraint foo check (test2>0);
alter table atacc3 inherit atacc2;
-- fail due to duplicates and circular inheritance
alter table atacc3 inherit atacc2;
alter table atacc2 inherit atacc3;
alter table atacc2 inherit atacc2;
-- test that we really are a child now (should see 4 not 3 and cascade should go through)
select test2 from atacc2;
drop table atacc2 cascade;
drop table atacc1;

-- adding only to a parent is allowed as of 9.2

create table atacc1 (test int);
create table atacc2 (test2 int) inherits (atacc1);
-- ok:
alter table atacc1 add constraint foo check (test>0) no inherit;
-- check constraint is not there on child
insert into atacc2 (test) values (-3);
-- check constraint is there on parent
insert into atacc1 (test) values (-3);
insert into atacc1 (test) values (3);
-- fail, violating row:
alter table atacc2 add constraint foo check (test>0) no inherit;
drop table atacc2;
drop table atacc1;

-- test unique constraint adding

create table atacc1 ( test int ) with oids;
-- add a unique constraint
alter table atacc1 add constraint atacc_test1 unique (test);
-- insert first value
insert into atacc1 (test) values (2);
-- should fail
insert into atacc1 (test) values (2);
-- should succeed
insert into atacc1 (test) values (4);
-- try adding a unique oid constraint
alter table atacc1 add constraint atacc_oid1 unique(oid);
-- try to create duplicates via alter table using - should fail
alter table atacc1 alter column test type integer using 0;
drop table atacc1;

-- let's do one where the unique constraint fails when added
create table atacc1 ( test int );
-- insert soon to be failing rows
insert into atacc1 (test) values (2);
insert into atacc1 (test) values (2);
-- add a unique constraint (fails)
alter table atacc1 add constraint atacc_test1 unique (test);
insert into atacc1 (test) values (3);
drop table atacc1;

-- let's do one where the unique constraint fails
-- because the column doesn't exist
create table atacc1 ( test int );
-- add a unique constraint (fails)
alter table atacc1 add constraint atacc_test1 unique (test1);
drop table atacc1;

-- something a little more complicated
create table atacc1 ( test int, test2 int);
-- add a unique constraint
alter table atacc1 add constraint atacc_test1 unique (test, test2);
-- insert initial value
insert into atacc1 (test,test2) values (4,4);
-- should fail
insert into atacc1 (test,test2) values (4,4);
-- should all succeed
insert into atacc1 (test,test2) values (4,5);
insert into atacc1 (test,test2) values (5,4);
insert into atacc1 (test,test2) values (5,5);
drop table atacc1;

-- lets do some naming tests
create table atacc1 (test int, test2 int, unique(test));
alter table atacc1 add unique (test2);
-- should fail for @@ second one @@
insert into atacc1 (test2, test) values (3, 3);
insert into atacc1 (test2, test) values (2, 3);
drop table atacc1;

-- test primary key constraint adding

create table atacc1 ( test int ) with oids;
-- add a primary key constraint
alter table atacc1 add constraint atacc_test1 primary key (test);
-- insert first value
insert into atacc1 (test) values (2);
-- should fail
insert into atacc1 (test) values (2);
-- should succeed
insert into atacc1 (test) values (4);
-- inserting NULL should fail
insert into atacc1 (test) values(NULL);
-- try adding a second primary key (should fail)
alter table atacc1 add constraint atacc_oid1 primary key(oid);
-- drop first primary key constraint
alter table atacc1 drop constraint atacc_test1 restrict;
-- try adding a primary key on oid (should succeed)
alter table atacc1 add constraint atacc_oid1 primary key(oid);
drop table atacc1;

-- let's do one where the primary key constraint fails when added
create table atacc1 ( test int );
-- insert soon to be failing rows
insert into atacc1 (test) values (2);
insert into atacc1 (test) values (2);
-- add a primary key (fails)
alter table atacc1 add constraint atacc_test1 primary key (test);
insert into atacc1 (test) values (3);
drop table atacc1;

-- let's do another one where the primary key constraint fails when added
create table atacc1 ( test int );
-- insert soon to be failing row
insert into atacc1 (test) values (NULL);
-- add a primary key (fails)
alter table atacc1 add constraint atacc_test1 primary key (test);
insert into atacc1 (test) values (3);
drop table atacc1;

-- let's do one where the primary key constraint fails
-- because the column doesn't exist
create table atacc1 ( test int );
-- add a primary key constraint (fails)
alter table atacc1 add constraint atacc_test1 primary key (test1);
drop table atacc1;

-- adding a new column as primary key to a non-empty table.
-- should fail unless the column has a non-null default value.
create table atacc1 ( test int );
insert into atacc1 (test) values (0);
-- add a primary key column without a default (fails).
alter table atacc1 add column test2 int primary key;
-- now add a primary key column with a default (succeeds).
alter table atacc1 add column test2 int default 0 primary key;
drop table atacc1;

-- something a little more complicated
create table atacc1 ( test int, test2 int);
-- add a primary key constraint
alter table atacc1 add constraint atacc_test1 primary key (test, test2);
-- try adding a second primary key - should fail
alter table atacc1 add constraint atacc_test2 primary key (test);
-- insert initial value
insert into atacc1 (test,test2) values (4,4);
-- should fail
insert into atacc1 (test,test2) values (4,4);
insert into atacc1 (test,test2) values (NULL,3);
insert into atacc1 (test,test2) values (3, NULL);
insert into atacc1 (test,test2) values (NULL,NULL);
-- should all succeed
insert into atacc1 (test,test2) values (4,5);
insert into atacc1 (test,test2) values (5,4);
insert into atacc1 (test,test2) values (5,5);
drop table atacc1;

-- lets do some naming tests
create table atacc1 (test int, test2 int, primary key(test));
-- only first should succeed
insert into atacc1 (test2, test) values (3, 3);
insert into atacc1 (test2, test) values (2, 3);
insert into atacc1 (test2, test) values (1, NULL);
drop table atacc1;

-- alter table / alter column [set/drop] not null tests
-- try altering system catalogs, should fail
alter table pg_class alter column relname drop not null;
alter table pg_class alter relname set not null;

-- try altering non-existent table, should fail
alter table non_existent alter column bar set not null;
alter table non_existent alter column bar drop not null;

-- test setting columns to null and not null and vice versa
-- test checking for null values and primary key
create table atacc1 (test int not null) with oids;
alter table atacc1 add constraint "atacc1_pkey" primary key (test);
alter table atacc1 alter column test drop not null;
alter table atacc1 drop constraint "atacc1_pkey";
alter table atacc1 alter column test drop not null;
insert into atacc1 values (null);
alter table atacc1 alter test set not null;
delete from atacc1;
alter table atacc1 alter test set not null;

-- try altering a non-existent column, should fail
alter table atacc1 alter bar set not null;
alter table atacc1 alter bar drop not null;

-- try altering the oid column, should fail
alter table atacc1 alter oid set not null;
alter table atacc1 alter oid drop not null;

-- try creating a view and altering that, should fail
create view myview as select * from atacc1;
alter table myview alter column test drop not null;
alter table myview alter column test set not null;
drop view myview;

drop table atacc1;

-- test inheritance
create table parent (a int);
create table child (b varchar(255)) inherits (parent);

alter table parent alter a set not null;
insert into parent values (NULL);
insert into child (a, b) values (NULL, 'foo');
alter table parent alter a drop not null;
insert into parent values (NULL);
insert into child (a, b) values (NULL, 'foo');
alter table only parent alter a set not null;
alter table child alter a set not null;
delete from parent;
alter table only parent alter a set not null;
insert into parent values (NULL);
alter table child alter a set not null;
insert into child (a, b) values (NULL, 'foo');
delete from child;
alter table child alter a set not null;
insert into child (a, b) values (NULL, 'foo');
drop table child;
drop table parent;

-- test setting and removing default values
create table def_test (
	c1	int4 default 5,
	c2	text default 'initial_default'
);
insert into def_test default values;
alter table def_test alter column c1 drop default;
insert into def_test default values;
alter table def_test alter column c2 drop default;
insert into def_test default values;
alter table def_test alter column c1 set default 10;
alter table def_test alter column c2 set default 'new_default';
insert into def_test default values;
select * from def_test;

-- set defaults to an incorrect type: this should fail
alter table def_test alter column c1 set default 'wrong_datatype';
alter table def_test alter column c2 set default 20;

-- set defaults on a non-existent column: this should fail
alter table def_test alter column c3 set default 30;

-- set defaults on views: we need to create a view, add a rule
-- to allow insertions into it, and then alter the view to add
-- a default
create view def_view_test as select * from def_test;
create rule def_view_test_ins as
	on insert to def_view_test
	do instead insert into def_test select new.*;
insert into def_view_test default values;
alter table def_view_test alter column c1 set default 45;
insert into def_view_test default values;
alter table def_view_test alter column c2 set default 'view_default';
insert into def_view_test default values;
select * from def_view_test;

drop rule def_view_test_ins on def_view_test;
drop view def_view_test;
drop table def_test;

-- alter table / drop column tests
-- try altering system catalogs, should fail
alter table pg_class drop column relname;

-- try altering non-existent table, should fail
alter table nosuchtable drop column bar;

-- test dropping columns
create table atacc1 (a int4 not null, b int4, c int4 not null, d int4) with oids;
insert into atacc1 values (1, 2, 3, 4);
alter table atacc1 drop a;
alter table atacc1 drop a;

-- SELECTs
select * from atacc1;
select * from atacc1 order by a;
select * from atacc1 order by "........pg.dropped.1........";
select * from atacc1 group by a;
select * from atacc1 group by "........pg.dropped.1........";
select atacc1.* from atacc1;
select a from atacc1;
select atacc1.a from atacc1;
select b,c,d from atacc1;
select a,b,c,d from atacc1;
select * from atacc1 where a = 1;
select "........pg.dropped.1........" from atacc1;
select atacc1."........pg.dropped.1........" from atacc1;
select "........pg.dropped.1........",b,c,d from atacc1;
select * from atacc1 where "........pg.dropped.1........" = 1;

-- UPDATEs
update atacc1 set a = 3;
update atacc1 set b = 2 where a = 3;
update atacc1 set "........pg.dropped.1........" = 3;
update atacc1 set b = 2 where "........pg.dropped.1........" = 3;

-- INSERTs
insert into atacc1 values (10, 11, 12, 13);
insert into atacc1 values (default, 11, 12, 13);
insert into atacc1 values (11, 12, 13);
insert into atacc1 (a) values (10);
insert into atacc1 (a) values (default);
insert into atacc1 (a,b,c,d) values (10,11,12,13);
insert into atacc1 (a,b,c,d) values (default,11,12,13);
insert into atacc1 (b,c,d) values (11,12,13);
insert into atacc1 ("........pg.dropped.1........") values (10);
insert into atacc1 ("........pg.dropped.1........") values (default);
insert into atacc1 ("........pg.dropped.1........",b,c,d) values (10,11,12,13);
insert into atacc1 ("........pg.dropped.1........",b,c,d) values (default,11,12,13);

-- DELETEs
delete from atacc1 where a = 3;
delete from atacc1 where "........pg.dropped.1........" = 3;
delete from atacc1;

-- try dropping a non-existent column, should fail
alter table atacc1 drop bar;

-- try dropping the oid column, should succeed
alter table atacc1 drop oid;

-- try dropping the xmin column, should fail
alter table atacc1 drop xmin;

-- try creating a view and altering that, should fail
create view myview as select * from atacc1;
select * from myview;
alter table myview drop d;
drop view myview;

-- test some commands to make sure they fail on the dropped column
analyze atacc1(a);
analyze atacc1("........pg.dropped.1........");
vacuum analyze atacc1(a);
vacuum analyze atacc1("........pg.dropped.1........");
comment on column atacc1.a is 'testing';
comment on column atacc1."........pg.dropped.1........" is 'testing';
alter table atacc1 alter a set storage plain;
alter table atacc1 alter "........pg.dropped.1........" set storage plain;
alter table atacc1 alter a set statistics 0;
alter table atacc1 alter "........pg.dropped.1........" set statistics 0;
alter table atacc1 alter a set default 3;
alter table atacc1 alter "........pg.dropped.1........" set default 3;
alter table atacc1 alter a drop default;
alter table atacc1 alter "........pg.dropped.1........" drop default;
alter table atacc1 alter a set not null;
alter table atacc1 alter "........pg.dropped.1........" set not null;
alter table atacc1 alter a drop not null;
alter table atacc1 alter "........pg.dropped.1........" drop not null;
alter table atacc1 rename a to x;
alter table atacc1 rename "........pg.dropped.1........" to x;
alter table atacc1 add primary key(a);
alter table atacc1 add primary key("........pg.dropped.1........");
alter table atacc1 add unique(a);
alter table atacc1 add unique("........pg.dropped.1........");
alter table atacc1 add check (a > 3);
alter table atacc1 add check ("........pg.dropped.1........" > 3);
create table atacc2 (id int4 unique);
alter table atacc1 add foreign key (a) references atacc2(id);
alter table atacc1 add foreign key ("........pg.dropped.1........") references atacc2(id);
alter table atacc2 add foreign key (id) references atacc1(a);
alter table atacc2 add foreign key (id) references atacc1("........pg.dropped.1........");
drop table atacc2;
create index "testing_idx" on atacc1(a);
create index "testing_idx" on atacc1("........pg.dropped.1........");

-- test create as and select into
insert into atacc1 values (21, 22, 23);
create table test1 as select * from atacc1;
select * from test1;
drop table test1;
select * into test2 from atacc1;
select * from test2;
drop table test2;

-- try dropping all columns
alter table atacc1 drop c;
alter table atacc1 drop d;
alter table atacc1 drop b;
select * from atacc1;

drop table atacc1;

-- test constraint error reporting in presence of dropped columns
create table atacc1 (id serial primary key, value int check (value < 10));
insert into atacc1(value) values (100);
alter table atacc1 drop column value;
alter table atacc1 add column value int check (value < 10);
insert into atacc1(value) values (100);
insert into atacc1(id, value) values (null, 0);
drop table atacc1;

-- test inheritance
create table parent (a int, b int, c int);
insert into parent values (1, 2, 3);
alter table parent drop a;
create table child (d varchar(255)) inherits (parent);
insert into child values (12, 13, 'testing');

select * from parent;
select * from child;
alter table parent drop c;
select * from parent;
select * from child;

drop table child;
drop table parent;

-- test copy in/out
create table test (a int4, b int4, c int4);
insert into test values (1,2,3);
alter table test drop a;
copy test to stdout;
copy test(a) to stdout;
copy test("........pg.dropped.1........") to stdout;
copy test from stdin;
10	11	12
\.
select * from test;
copy test from stdin;
21	22
\.
select * from test;
copy test(a) from stdin;
copy test("........pg.dropped.1........") from stdin;
copy test(b,c) from stdin;
31	32
\.
select * from test;
drop table test;

-- test inheritance

create table dropColumn (a int, b int, e int);
create table dropColumnChild (c int) inherits (dropColumn);
create table dropColumnAnother (d int) inherits (dropColumnChild);

-- these two should fail
alter table dropColumnchild drop column a;
alter table only dropColumnChild drop column b;



-- these three should work
alter table only dropColumn drop column e;
alter table dropColumnChild drop column c;
alter table dropColumn drop column a;

create table renameColumn (a int);
create table renameColumnChild (b int) inherits (renameColumn);
create table renameColumnAnother (c int) inherits (renameColumnChild);

-- these three should fail
alter table renameColumnChild rename column a to d;
alter table only renameColumnChild rename column a to d;
alter table only renameColumn rename column a to d;

-- these should work
alter table renameColumn rename column a to d;
alter table renameColumnChild rename column b to a;

-- these should work
alter table if exists doesnt_exist_tab rename column a to d;
alter table if exists doesnt_exist_tab rename column b to a;

-- this should work
alter table renameColumn add column w int;

-- this should fail
alter table only renameColumn add column x int;


-- Test corner cases in dropping of inherited columns

create table p1 (f1 int, f2 int);
create table c1 (f1 int not null) inherits(p1);

-- should be rejected since c1.f1 is inherited
alter table c1 drop column f1;
-- should work
alter table p1 drop column f1;
-- c1.f1 is still there, but no longer inherited
select f1 from c1;
alter table c1 drop column f1;
select f1 from c1;

drop table p1 cascade;

create table p1 (f1 int, f2 int);
create table c1 () inherits(p1);

-- should be rejected since c1.f1 is inherited
alter table c1 drop column f1;
alter table p1 drop column f1;
-- c1.f1 is dropped now, since there is no local definition for it
select f1 from c1;

drop table p1 cascade;

create table p1 (f1 int, f2 int);
create table c1 () inherits(p1);

-- should be rejected since c1.f1 is inherited
alter table c1 drop column f1;
alter table only p1 drop column f1;
-- c1.f1 is NOT dropped, but must now be considered non-inherited
alter table c1 drop column f1;

drop table p1 cascade;

create table p1 (f1 int, f2 int);
create table c1 (f1 int not null) inherits(p1);

-- should be rejected since c1.f1 is inherited
alter table c1 drop column f1;
alter table only p1 drop column f1;
-- c1.f1 is still there, but no longer inherited
alter table c1 drop column f1;

drop table p1 cascade;

create table p1(id int, name text);
create table p2(id2 int, name text, height int);
create table c1(age int) inherits(p1,p2);
create table gc1() inherits (c1);

select relname, attname, attinhcount, attislocal
from pg_class join pg_attribute on (pg_class.oid = pg_attribute.attrelid)
where relname in ('p1','p2','c1','gc1') and attnum > 0 and not attisdropped
order by relname, attnum;

-- should work
alter table only p1 drop column name;
-- should work. Now c1.name is local and inhcount is 0.
alter table p2 drop column name;
-- should be rejected since its inherited
alter table gc1 drop column name;
-- should work, and drop gc1.name along
alter table c1 drop column name;
-- should fail: column does not exist
alter table gc1 drop column name;
-- should work and drop the attribute in all tables
alter table p2 drop column height;

-- IF EXISTS test
create table dropColumnExists ();
alter table dropColumnExists drop column non_existing; --fail
alter table dropColumnExists drop column if exists non_existing; --succeed

select relname, attname, attinhcount, attislocal
from pg_class join pg_attribute on (pg_class.oid = pg_attribute.attrelid)
where relname in ('p1','p2','c1','gc1') and attnum > 0 and not attisdropped
order by relname, attnum;

drop table p1, p2 cascade;

-- test attinhcount tracking with merged columns

create table depth0();
create table depth1(c text) inherits (depth0);
create table depth2() inherits (depth1);
alter table depth0 add c text;

select attrelid::regclass, attname, attinhcount, attislocal
from pg_attribute
where attnum > 0 and attrelid::regclass in ('depth0', 'depth1', 'depth2')
order by attrelid::regclass::text, attnum;

--
-- Test the ALTER TABLE SET WITH/WITHOUT OIDS command
--
create table altstartwith (col integer) with oids;

insert into altstartwith values (1);

select oid > 0, * from altstartwith;

alter table altstartwith set without oids;

select oid > 0, * from altstartwith; -- fails
select * from altstartwith;

alter table altstartwith set with oids;

select oid > 0, * from altstartwith;

drop table altstartwith;

-- Check inheritance cases
create table altwithoid (col integer) with oids;

-- Inherits parents oid column anyway
create table altinhoid () inherits (altwithoid) without oids;

insert into altinhoid values (1);

select oid > 0, * from altwithoid;
select oid > 0, * from altinhoid;

alter table altwithoid set without oids;

select oid > 0, * from altwithoid; -- fails
select oid > 0, * from altinhoid; -- fails
select * from altwithoid;
select * from altinhoid;

alter table altwithoid set with oids;

select oid > 0, * from altwithoid;
select oid > 0, * from altinhoid;

drop table altwithoid cascade;

create table altwithoid (col integer) without oids;

-- child can have local oid column
create table altinhoid () inherits (altwithoid) with oids;

insert into altinhoid values (1);

select oid > 0, * from altwithoid; -- fails
select oid > 0, * from altinhoid;

alter table altwithoid set with oids;

select oid > 0, * from altwithoid;
select oid > 0, * from altinhoid;

-- the child's local definition should remain
alter table altwithoid set without oids;

select oid > 0, * from altwithoid; -- fails
select oid > 0, * from altinhoid;

drop table altwithoid cascade;

-- test renumbering of child-table columns in inherited operations

create table p1 (f1 int);
create table c1 (f2 text, f3 int) inherits (p1);

alter table p1 add column a1 int check (a1 > 0);
alter table p1 add column f2 text;

insert into p1 values (1,2,'abc');
insert into c1 values(11,'xyz',33,0); -- should fail
insert into c1 values(11,'xyz',33,22);

select * from p1;
update p1 set a1 = a1 + 1, f2 = upper(f2);
select * from p1;

drop table p1 cascade;

-- test that operations with a dropped column do not try to reference
-- its datatype

create domain mytype as text;
create temp table foo (f1 text, f2 mytype, f3 text);

insert into foo values('bb','cc','dd');
select * from foo;

drop domain mytype cascade;

select * from foo;
insert into foo values('qq','rr');
select * from foo;
update foo set f3 = 'zz';
select * from foo;
select f3,max(f1) from foo group by f3;

-- Simple tests for alter table column type
alter table foo alter f1 TYPE integer; -- fails
alter table foo alter f1 TYPE varchar(10);

create table anothertab (atcol1 serial8, atcol2 boolean,
	constraint anothertab_chk check (atcol1 <= 3));

insert into anothertab (atcol1, atcol2) values (default, true);
insert into anothertab (atcol1, atcol2) values (default, false);
select * from anothertab;

alter table anothertab alter column atcol1 type boolean; -- fails
alter table anothertab alter column atcol1 type boolean using atcol1::int; -- fails
alter table anothertab alter column atcol1 type integer;

select * from anothertab;

insert into anothertab (atcol1, atcol2) values (45, null); -- fails
insert into anothertab (atcol1, atcol2) values (default, null);

select * from anothertab;

alter table anothertab alter column atcol2 type text
      using case when atcol2 is true then 'IT WAS TRUE'
                 when atcol2 is false then 'IT WAS FALSE'
                 else 'IT WAS NULL!' end;

select * from anothertab;
alter table anothertab alter column atcol1 type boolean
        using case when atcol1 % 2 = 0 then true else false end; -- fails
alter table anothertab alter column atcol1 drop default;
alter table anothertab alter column atcol1 type boolean
        using case when atcol1 % 2 = 0 then true else false end; -- fails
alter table anothertab drop constraint anothertab_chk;
alter table anothertab drop constraint anothertab_chk; -- fails
alter table anothertab drop constraint IF EXISTS anothertab_chk; -- succeeds

alter table anothertab alter column atcol1 type boolean
        using case when atcol1 % 2 = 0 then true else false end;

select * from anothertab;

drop table anothertab;

create table another (f1 int, f2 text);

insert into another values(1, 'one');
insert into another values(2, 'two');
insert into another values(3, 'three');

select * from another;

alter table another
  alter f1 type text using f2 || ' more',
  alter f2 type bigint using f1 * 10;

select * from another;

drop table another;

-- table's row type
create table tab1 (a int, b text);
create table tab2 (x int, y tab1);
alter table tab1 alter column b type varchar; -- fails

-- disallow recursive containment of row types
create temp table recur1 (f1 int);
alter table recur1 add column f2 recur1; -- fails
alter table recur1 add column f2 recur1[]; -- fails
create domain array_of_recur1 as recur1[];
alter table recur1 add column f2 array_of_recur1; -- fails
create temp table recur2 (f1 int, f2 recur1);
alter table recur1 add column f2 recur2; -- fails
alter table recur1 add column f2 int;
alter table recur1 alter column f2 type recur2; -- fails

-- SET STORAGE may need to add a TOAST table
create table test_storage (a text);
alter table test_storage alter a set storage plain;
alter table test_storage add b int default 0; -- rewrite table to remove its TOAST table
alter table test_storage alter a set storage extended; -- re-add TOAST table

select reltoastrelid <> 0 as has_toast_table
from pg_class
where oid = 'test_storage'::regclass;

-- ALTER COLUMN TYPE with a check constraint and a child table (bug #13779)
CREATE TABLE test_inh_check (a float check (a > 10.2), b float);
CREATE TABLE test_inh_check_child() INHERITS(test_inh_check);
\d test_inh_check
\d test_inh_check_child
select relname, conname, coninhcount, conislocal, connoinherit
  from pg_constraint c, pg_class r
  where relname like 'test_inh_check%' and c.conrelid = r.oid
  order by 1, 2;
ALTER TABLE test_inh_check ALTER COLUMN a TYPE numeric;
\d test_inh_check
\d test_inh_check_child
select relname, conname, coninhcount, conislocal, connoinherit
  from pg_constraint c, pg_class r
  where relname like 'test_inh_check%' and c.conrelid = r.oid
  order by 1, 2;
-- also try noinherit, local, and local+inherited cases
ALTER TABLE test_inh_check ADD CONSTRAINT bnoinherit CHECK (b > 100) NO INHERIT;
ALTER TABLE test_inh_check_child ADD CONSTRAINT blocal CHECK (b < 1000);
ALTER TABLE test_inh_check_child ADD CONSTRAINT bmerged CHECK (b > 1);
ALTER TABLE test_inh_check ADD CONSTRAINT bmerged CHECK (b > 1);
\d test_inh_check
\d test_inh_check_child
select relname, conname, coninhcount, conislocal, connoinherit
  from pg_constraint c, pg_class r
  where relname like 'test_inh_check%' and c.conrelid = r.oid
  order by 1, 2;
ALTER TABLE test_inh_check ALTER COLUMN b TYPE numeric;
\d test_inh_check
\d test_inh_check_child
select relname, conname, coninhcount, conislocal, connoinherit
  from pg_constraint c, pg_class r
  where relname like 'test_inh_check%' and c.conrelid = r.oid
  order by 1, 2;

-- check for rollback of ANALYZE corrupting table property flags (bug #11638)
CREATE TABLE check_fk_presence_1 (id int PRIMARY KEY, t text);
CREATE TABLE check_fk_presence_2 (id int REFERENCES check_fk_presence_1, t text);
BEGIN;
ALTER TABLE check_fk_presence_2 DROP CONSTRAINT check_fk_presence_2_id_fkey;
ANALYZE check_fk_presence_2;
ROLLBACK;
\d check_fk_presence_2
DROP TABLE check_fk_presence_1, check_fk_presence_2;

--
-- lock levels
--
drop type lockmodes;
create type lockmodes as enum (
 'SIReadLock'
,'AccessShareLock'
,'RowShareLock'
,'RowExclusiveLock'
,'ShareUpdateExclusiveLock'
,'ShareLock'
,'ShareRowExclusiveLock'
,'ExclusiveLock'
,'AccessExclusiveLock'
);

drop view my_locks;
create or replace view my_locks as
select case when c.relname like 'pg_toast%' then 'pg_toast' else c.relname end, max(mode::lockmodes) as max_lockmode
from pg_locks l join pg_class c on l.relation = c.oid
where virtualtransaction = (
        select virtualtransaction
        from pg_locks
        where transactionid = txid_current()::integer)
and locktype = 'relation'
and relnamespace != (select oid from pg_namespace where nspname = 'pg_catalog')
and c.relname != 'my_locks'
group by c.relname;

create table alterlock (f1 int primary key, f2 text);
insert into alterlock values (1, 'foo');
create table alterlock2 (f3 int primary key, f1 int);
insert into alterlock2 values (1, 1);

begin; alter table alterlock alter column f2 set statistics 150;
select * from my_locks order by 1;
rollback;

begin; alter table alterlock cluster on alterlock_pkey;
select * from my_locks order by 1;
commit;

begin; alter table alterlock set without cluster;
select * from my_locks order by 1;
commit;

begin; alter table alterlock set (fillfactor = 100);
select * from my_locks order by 1;
commit;

begin; alter table alterlock reset (fillfactor);
select * from my_locks order by 1;
commit;

begin; alter table alterlock set (toast.autovacuum_enabled = off);
select * from my_locks order by 1;
commit;

begin; alter table alterlock set (autovacuum_enabled = off);
select * from my_locks order by 1;
commit;

begin; alter table alterlock alter column f2 set (n_distinct = 1);
select * from my_locks order by 1;
rollback;

begin; alter table alterlock alter column f2 set storage extended;
select * from my_locks order by 1;
rollback;

begin; alter table alterlock alter column f2 set default 'x';
select * from my_locks order by 1;
rollback;

begin;
create trigger ttdummy
	before delete or update on alterlock
	for each row
	execute procedure
	ttdummy (1, 1);
select * from my_locks order by 1;
rollback;

begin;
select * from my_locks order by 1;
alter table alterlock2 add foreign key (f1) references alterlock (f1);
select * from my_locks order by 1;
rollback;

begin;
alter table alterlock2
add constraint alterlock2nv foreign key (f1) references alterlock (f1) NOT VALID;
select * from my_locks order by 1;
commit;
begin;
alter table alterlock2 validate constraint alterlock2nv;
select * from my_locks order by 1;
rollback;

-- cleanup
drop table alterlock2;
drop table alterlock;
drop view my_locks;
drop type lockmodes;

--
-- alter function
--
create function test_strict(text) returns text as
    'select coalesce($1, ''got passed a null'');'
    language sql returns null on null input;
select test_strict(NULL);
alter function test_strict(text) called on null input;
select test_strict(NULL);

create function non_strict(text) returns text as
    'select coalesce($1, ''got passed a null'');'
    language sql called on null input;
select non_strict(NULL);
alter function non_strict(text) returns null on null input;
select non_strict(NULL);

--
-- alter object set schema
--

create schema alter1;
create schema alter2;

create table alter1.t1(f1 serial primary key, f2 int check (f2 > 0));

create view alter1.v1 as select * from alter1.t1;

create function alter1.plus1(int) returns int as 'select $1+1' language sql;

create domain alter1.posint integer check (value > 0);

create type alter1.ctype as (f1 int, f2 text);

create function alter1.same(alter1.ctype, alter1.ctype) returns boolean language sql
as 'select $1.f1 is not distinct from $2.f1 and $1.f2 is not distinct from $2.f2';

create operator alter1.=(procedure = alter1.same, leftarg  = alter1.ctype, rightarg = alter1.ctype);

create operator class alter1.ctype_hash_ops default for type alter1.ctype using hash as
  operator 1 alter1.=(alter1.ctype, alter1.ctype);

create conversion alter1.ascii_to_utf8 for 'sql_ascii' to 'utf8' from ascii_to_utf8;

create text search parser alter1.prs(start = prsd_start, gettoken = prsd_nexttoken, end = prsd_end, lextypes = prsd_lextype);
create text search configuration alter1.cfg(parser = alter1.prs);
create text search template alter1.tmpl(init = dsimple_init, lexize = dsimple_lexize);
create text search dictionary alter1.dict(template = alter1.tmpl);

insert into alter1.t1(f2) values(11);
insert into alter1.t1(f2) values(12);

alter table alter1.t1 set schema alter2;
alter table alter1.v1 set schema alter2;
alter function alter1.plus1(int) set schema alter2;
alter domain alter1.posint set schema alter2;
alter operator class alter1.ctype_hash_ops using hash set schema alter2;
alter operator family alter1.ctype_hash_ops using hash set schema alter2;
alter operator alter1.=(alter1.ctype, alter1.ctype) set schema alter2;
alter function alter1.same(alter1.ctype, alter1.ctype) set schema alter2;
alter type alter1.ctype set schema alter2;
alter conversion alter1.ascii_to_utf8 set schema alter2;
alter text search parser alter1.prs set schema alter2;
alter text search configuration alter1.cfg set schema alter2;
alter text search template alter1.tmpl set schema alter2;
alter text search dictionary alter1.dict set schema alter2;

-- this should succeed because nothing is left in alter1
drop schema alter1;

insert into alter2.t1(f2) values(13);
insert into alter2.t1(f2) values(14);

select * from alter2.t1;

select * from alter2.v1;

select alter2.plus1(41);

-- clean up
drop schema alter2 cascade;

--
-- composite types
--

CREATE TYPE test_type AS (a int);
\d test_type

ALTER TYPE nosuchtype ADD ATTRIBUTE b text; -- fails

ALTER TYPE test_type ADD ATTRIBUTE b text;
\d test_type

ALTER TYPE test_type ADD ATTRIBUTE b text; -- fails

ALTER TYPE test_type ALTER ATTRIBUTE b SET DATA TYPE varchar;
\d test_type

ALTER TYPE test_type ALTER ATTRIBUTE b SET DATA TYPE integer;
\d test_type

ALTER TYPE test_type DROP ATTRIBUTE b;
\d test_type

ALTER TYPE test_type DROP ATTRIBUTE c; -- fails

ALTER TYPE test_type DROP ATTRIBUTE IF EXISTS c;

ALTER TYPE test_type DROP ATTRIBUTE a, ADD ATTRIBUTE d boolean;
\d test_type

ALTER TYPE test_type RENAME ATTRIBUTE a TO aa;
ALTER TYPE test_type RENAME ATTRIBUTE d TO dd;
\d test_type

DROP TYPE test_type;

CREATE TYPE test_type1 AS (a int, b text);
CREATE TABLE test_tbl1 (x int, y test_type1);
ALTER TYPE test_type1 ALTER ATTRIBUTE b TYPE varchar; -- fails

CREATE TYPE test_type2 AS (a int, b text);
CREATE TABLE test_tbl2 OF test_type2;
CREATE TABLE test_tbl2_subclass () INHERITS (test_tbl2);
\d test_type2
\d test_tbl2

ALTER TYPE test_type2 ADD ATTRIBUTE c text; -- fails
ALTER TYPE test_type2 ADD ATTRIBUTE c text CASCADE;
\d test_type2
\d test_tbl2

ALTER TYPE test_type2 ALTER ATTRIBUTE b TYPE varchar; -- fails
ALTER TYPE test_type2 ALTER ATTRIBUTE b TYPE varchar CASCADE;
\d test_type2
\d test_tbl2

ALTER TYPE test_type2 DROP ATTRIBUTE b; -- fails
ALTER TYPE test_type2 DROP ATTRIBUTE b CASCADE;
\d test_type2
\d test_tbl2

ALTER TYPE test_type2 RENAME ATTRIBUTE a TO aa; -- fails
ALTER TYPE test_type2 RENAME ATTRIBUTE a TO aa CASCADE;
\d test_type2
\d test_tbl2
\d test_tbl2_subclass

DROP TABLE test_tbl2_subclass;

-- This test isn't that interesting on its own, but the purpose is to leave
-- behind a table to test pg_upgrade with. The table has a composite type
-- column in it, and the composite type has a dropped attribute.
CREATE TYPE test_type3 AS (a int);
CREATE TABLE test_tbl3 (c) AS SELECT '(1)'::test_type3;
ALTER TYPE test_type3 DROP ATTRIBUTE a, ADD ATTRIBUTE b int;

CREATE TYPE test_type_empty AS ();
DROP TYPE test_type_empty;

--
-- typed tables: OF / NOT OF
--

CREATE TYPE tt_t0 AS (z inet, x int, y numeric(8,2));
ALTER TYPE tt_t0 DROP ATTRIBUTE z;
CREATE TABLE tt0 (x int NOT NULL, y numeric(8,2));	-- OK
CREATE TABLE tt1 (x int, y bigint);					-- wrong base type
CREATE TABLE tt2 (x int, y numeric(9,2));			-- wrong typmod
CREATE TABLE tt3 (y numeric(8,2), x int);			-- wrong column order
CREATE TABLE tt4 (x int);							-- too few columns
CREATE TABLE tt5 (x int, y numeric(8,2), z int);	-- too few columns
CREATE TABLE tt6 () INHERITS (tt0);					-- can't have a parent
CREATE TABLE tt7 (x int, q text, y numeric(8,2)) WITH OIDS;
ALTER TABLE tt7 DROP q;								-- OK

ALTER TABLE tt0 OF tt_t0;
ALTER TABLE tt1 OF tt_t0;
ALTER TABLE tt2 OF tt_t0;
ALTER TABLE tt3 OF tt_t0;
ALTER TABLE tt4 OF tt_t0;
ALTER TABLE tt5 OF tt_t0;
ALTER TABLE tt6 OF tt_t0;
ALTER TABLE tt7 OF tt_t0;

CREATE TYPE tt_t1 AS (x int, y numeric(8,2));
ALTER TABLE tt7 OF tt_t1;			-- reassign an already-typed table
ALTER TABLE tt7 NOT OF;
\d tt7

-- make sure we can drop a constraint on the parent but it remains on the child
CREATE TABLE test_drop_constr_parent (c text CHECK (c IS NOT NULL));
CREATE TABLE test_drop_constr_child () INHERITS (test_drop_constr_parent);
ALTER TABLE ONLY test_drop_constr_parent DROP CONSTRAINT "test_drop_constr_parent_c_check";
-- should fail
INSERT INTO test_drop_constr_child (c) VALUES (NULL);
DROP TABLE test_drop_constr_parent CASCADE;

--
-- IF EXISTS test
--
ALTER TABLE IF EXISTS tt8 ADD COLUMN f int;
ALTER TABLE IF EXISTS tt8 ADD CONSTRAINT xxx PRIMARY KEY(f);
ALTER TABLE IF EXISTS tt8 ADD CHECK (f BETWEEN 0 AND 10);
ALTER TABLE IF EXISTS tt8 ALTER COLUMN f SET DEFAULT 0;
ALTER TABLE IF EXISTS tt8 RENAME COLUMN f TO f1;
ALTER TABLE IF EXISTS tt8 SET SCHEMA alter2;

CREATE TABLE tt8(a int);
CREATE SCHEMA alter2;

ALTER TABLE IF EXISTS tt8 ADD COLUMN f int;
ALTER TABLE IF EXISTS tt8 ADD CONSTRAINT xxx PRIMARY KEY(f);
ALTER TABLE IF EXISTS tt8 ADD CHECK (f BETWEEN 0 AND 10);
ALTER TABLE IF EXISTS tt8 ALTER COLUMN f SET DEFAULT 0;
ALTER TABLE IF EXISTS tt8 RENAME COLUMN f TO f1;
ALTER TABLE IF EXISTS tt8 SET SCHEMA alter2;

\d alter2.tt8

DROP TABLE alter2.tt8;
DROP SCHEMA alter2;


-- Check that comments on constraints and indexes are not lost at ALTER TABLE.
CREATE TABLE comment_test (
  id int,
  positive_col int CHECK (positive_col > 0),
  indexed_col int,
  CONSTRAINT comment_test_pk PRIMARY KEY (id));
CREATE INDEX comment_test_index ON comment_test(indexed_col);

COMMENT ON COLUMN comment_test.id IS 'Column ''id'' on comment_test';
COMMENT ON INDEX comment_test_index IS 'Simple index on comment_test';
COMMENT ON CONSTRAINT comment_test_positive_col_check ON comment_test IS 'CHECK constraint on comment_test.positive_col';
COMMENT ON CONSTRAINT comment_test_pk ON comment_test IS 'PRIMARY KEY constraint of comment_test';
COMMENT ON INDEX comment_test_pk IS 'Index backing the PRIMARY KEY of comment_test';

SELECT col_description('comment_test'::regclass, 1) as comment;
SELECT indexrelid::regclass::text as index, obj_description(indexrelid, 'pg_class') as comment FROM pg_index where indrelid = 'comment_test'::regclass ORDER BY 1, 2;
SELECT conname as constraint, obj_description(oid, 'pg_constraint') as comment FROM pg_constraint where conrelid = 'comment_test'::regclass ORDER BY 1, 2;

-- Change the datatype of all the columns. ALTER TABLE is optimized to not
-- rebuild an index if the new data type is binary compatible with the old
-- one. Check do a dummy ALTER TABLE that doesn't change the datatype
-- first, to test that no-op codepath, and another one that does.
ALTER TABLE comment_test ALTER COLUMN indexed_col SET DATA TYPE int;
ALTER TABLE comment_test ALTER COLUMN indexed_col SET DATA TYPE text;
ALTER TABLE comment_test ALTER COLUMN id SET DATA TYPE int;
ALTER TABLE comment_test ALTER COLUMN id SET DATA TYPE text;
ALTER TABLE comment_test ALTER COLUMN positive_col SET DATA TYPE int;
ALTER TABLE comment_test ALTER COLUMN positive_col SET DATA TYPE bigint;

-- Check that the comments are intact.
SELECT col_description('comment_test'::regclass, 1) as comment;
SELECT indexrelid::regclass::text as index, obj_description(indexrelid, 'pg_class') as comment FROM pg_index where indrelid = 'comment_test'::regclass ORDER BY 1, 2;
SELECT conname as constraint, obj_description(oid, 'pg_constraint') as comment FROM pg_constraint where conrelid = 'comment_test'::regclass ORDER BY 1, 2;


-- Check that we map relation oids to filenodes and back correctly.  Only
-- display bad mappings so the test output doesn't change all the time.  A
-- filenode function call can return NULL for a relation dropped concurrently
-- with the call's surrounding query, so ignore a NULL mapped_oid for
-- relations that no longer exist after all calls finish.
CREATE TEMP TABLE filenode_mapping AS
SELECT
    oid, mapped_oid, reltablespace, relfilenode, relname
FROM pg_class,
    pg_filenode_relation(reltablespace, pg_relation_filenode(oid)) AS mapped_oid
WHERE relkind IN ('r', 'i', 'S', 't', 'm') AND mapped_oid IS DISTINCT FROM oid;

SELECT m.* FROM filenode_mapping m LEFT JOIN pg_class c ON c.oid = m.oid
WHERE c.oid IS NOT NULL OR m.mapped_oid IS NOT NULL;

-- Checks on creating and manipulation of user defined relations in
-- pg_catalog.
--
-- XXX: It would be useful to add checks around trying to manipulate
-- catalog tables, but that might have ugly consequences when run
-- against an existing server with allow_system_table_mods = on.

SHOW allow_system_table_mods;
-- disallowed because of search_path issues with pg_dump
CREATE TABLE pg_catalog.new_system_table();
-- instead create in public first, move to catalog
CREATE TABLE new_system_table(id serial primary key, othercol text);
ALTER TABLE new_system_table SET SCHEMA pg_catalog;

-- XXX: it's currently impossible to move relations out of pg_catalog
ALTER TABLE new_system_table SET SCHEMA public;
-- move back, will currently error out, already there
ALTER TABLE new_system_table SET SCHEMA pg_catalog;
ALTER TABLE new_system_table RENAME TO old_system_table;
CREATE INDEX old_system_table__othercol ON old_system_table (othercol);
INSERT INTO old_system_table(othercol) VALUES ('somedata'), ('otherdata');
UPDATE old_system_table SET id = -id;
DELETE FROM old_system_table WHERE othercol = 'somedata';
TRUNCATE old_system_table;
ALTER TABLE old_system_table DROP CONSTRAINT new_system_table_pkey;
ALTER TABLE old_system_table DROP COLUMN othercol;
DROP TABLE old_system_table;

-- set logged
CREATE UNLOGGED TABLE unlogged1(f1 SERIAL PRIMARY KEY, f2 TEXT);
-- check relpersistence of an unlogged table
SELECT relname, relkind, relpersistence FROM pg_class WHERE relname ~ '^unlogged1'
UNION ALL
SELECT 'toast table', t.relkind, t.relpersistence FROM pg_class r JOIN pg_class t ON t.oid = r.reltoastrelid WHERE r.relname ~ '^unlogged1'
UNION ALL
SELECT 'toast index', ri.relkind, ri.relpersistence FROM pg_class r join pg_class t ON t.oid = r.reltoastrelid JOIN pg_index i ON i.indrelid = t.oid JOIN pg_class ri ON ri.oid = i.indexrelid WHERE r.relname ~ '^unlogged1'
ORDER BY relname;
CREATE UNLOGGED TABLE unlogged2(f1 SERIAL PRIMARY KEY, f2 INTEGER REFERENCES unlogged1); -- foreign key
CREATE UNLOGGED TABLE unlogged3(f1 SERIAL PRIMARY KEY, f2 INTEGER REFERENCES unlogged3); -- self-referencing foreign key
ALTER TABLE unlogged3 SET LOGGED; -- skip self-referencing foreign key
ALTER TABLE unlogged2 SET LOGGED; -- fails because a foreign key to an unlogged table exists
ALTER TABLE unlogged1 SET LOGGED;
-- check relpersistence of an unlogged table after changing to permament
SELECT relname, relkind, relpersistence FROM pg_class WHERE relname ~ '^unlogged1'
UNION ALL
SELECT 'toast table', t.relkind, t.relpersistence FROM pg_class r JOIN pg_class t ON t.oid = r.reltoastrelid WHERE r.relname ~ '^unlogged1'
UNION ALL
SELECT 'toast index', ri.relkind, ri.relpersistence FROM pg_class r join pg_class t ON t.oid = r.reltoastrelid JOIN pg_index i ON i.indrelid = t.oid JOIN pg_class ri ON ri.oid = i.indexrelid WHERE r.relname ~ '^unlogged1'
ORDER BY relname;
ALTER TABLE unlogged1 SET LOGGED; -- silently do nothing
DROP TABLE unlogged3;
DROP TABLE unlogged2;
DROP TABLE unlogged1;
-- set unlogged
CREATE TABLE logged1(f1 SERIAL PRIMARY KEY, f2 TEXT);
-- check relpersistence of a permanent table
SELECT relname, relkind, relpersistence FROM pg_class WHERE relname ~ '^logged1'
UNION ALL
SELECT 'toast table', t.relkind, t.relpersistence FROM pg_class r JOIN pg_class t ON t.oid = r.reltoastrelid WHERE r.relname ~ '^logged1'
UNION ALL
SELECT 'toast index', ri.relkind, ri.relpersistence FROM pg_class r join pg_class t ON t.oid = r.reltoastrelid JOIN pg_index i ON i.indrelid = t.oid JOIN pg_class ri ON ri.oid = i.indexrelid WHERE r.relname ~ '^logged1'
ORDER BY relname;
CREATE TABLE logged2(f1 SERIAL PRIMARY KEY, f2 INTEGER REFERENCES logged1); -- foreign key
CREATE TABLE logged3(f1 SERIAL PRIMARY KEY, f2 INTEGER REFERENCES logged3); -- self-referencing foreign key
ALTER TABLE logged1 SET UNLOGGED; -- fails because a foreign key from a permanent table exists
ALTER TABLE logged3 SET UNLOGGED; -- skip self-referencing foreign key
ALTER TABLE logged2 SET UNLOGGED;
ALTER TABLE logged1 SET UNLOGGED;
-- check relpersistence of a permanent table after changing to unlogged
SELECT relname, relkind, relpersistence FROM pg_class WHERE relname ~ '^logged1'
UNION ALL
SELECT 'toast table', t.relkind, t.relpersistence FROM pg_class r JOIN pg_class t ON t.oid = r.reltoastrelid WHERE r.relname ~ '^logged1'
UNION ALL
SELECT 'toast index', ri.relkind, ri.relpersistence FROM pg_class r join pg_class t ON t.oid = r.reltoastrelid JOIN pg_index i ON i.indrelid = t.oid JOIN pg_class ri ON ri.oid = i.indexrelid WHERE r.relname ~ '^logged1'
ORDER BY relname;
ALTER TABLE logged1 SET UNLOGGED; -- silently do nothing
DROP TABLE logged3;
DROP TABLE logged2;
DROP TABLE logged1;
