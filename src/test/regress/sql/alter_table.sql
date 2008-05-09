--
-- ALTER_TABLE
-- add attribute
--

CREATE TABLE tmp (initial int4);

COMMENT ON TABLE tmp_wrong IS 'table comment';
COMMENT ON TABLE tmp IS 'table comment';
COMMENT ON TABLE tmp IS NULL;

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
ALTER INDEX onek_unique1 RENAME TO tmp_onek_unique1;
ALTER INDEX tmp_onek_unique1 RENAME TO onek_unique1;
-- renaming views
CREATE VIEW tmp_view (unique1) AS SELECT unique1 FROM tenk1;
ALTER TABLE tmp_view RENAME TO tmp_view_new;

-- hack to ensure we get an indexscan here
ANALYZE tenk1;
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

-- Try (and fail) to create constraint from tmp5(a) to tmp4(a) - unique constraint on
-- tmp4 is a,b

ALTER TABLE tmp5 add constraint tmpconstr foreign key(a) references tmp4(a) match full;

DROP TABLE tmp5;

DROP TABLE tmp4;

DROP TABLE tmp3;

DROP TABLE tmp2;

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
alter table atacc3 add inherit atacc2;
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

-- adding only to a parent is disallowed as of 8.4

create table atacc1 (test int);
create table atacc2 (test2 int) inherits (atacc1);
-- fail:
alter table only atacc1 add constraint foo check (test>0);
-- ok:
alter table only atacc2 add constraint foo check (test>0);
-- check constraint not there on parent
insert into atacc1 (test) values (-3);
insert into atacc1 (test) values (3);
-- check constraint is there on child
insert into atacc2 (test) values (-3);
insert into atacc2 (test) values (3);
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

select relname, attname, attinhcount, attislocal
from pg_class join pg_attribute on (pg_class.oid = pg_attribute.attrelid)
where relname in ('p1','p2','c1','gc1') and attnum > 0 and not attisdropped
order by relname, attnum;

drop table p1, p2 cascade;

--
-- Test the ALTER TABLE WITHOUT OIDS command
--
create table altstartwith (col integer) with oids;

insert into altstartwith values (1);

select oid > 0, * from altstartwith;

alter table altstartwith set without oids;

select oid > 0, * from altstartwith; -- fails
select * from altstartwith;

-- Run inheritance tests
create table altwithoid (col integer) with oids;

-- Inherits parents oid column
create table altinhoid () inherits (altwithoid) without oids;

insert into altinhoid values (1);

select oid > 0, * from altwithoid;
select oid > 0, * from altinhoid;

alter table altwithoid set without oids;
alter table altinhoid set without oids;

select oid > 0, * from altwithoid; -- fails
select oid > 0, * from altinhoid; -- fails
select * from altwithoid;
select * from altinhoid;

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

insert into foo values('aa','bb','cc');
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

insert into alter1.t1(f2) values(11);
insert into alter1.t1(f2) values(12);

alter table alter1.t1 set schema alter2;
alter table alter1.v1 set schema alter2;
alter function alter1.plus1(int) set schema alter2;
alter domain alter1.posint set schema alter2;
alter type alter1.ctype set schema alter2;

-- this should succeed because nothing is left in alter1
drop schema alter1;

insert into alter2.t1(f2) values(13);
insert into alter2.t1(f2) values(14);

select * from alter2.t1;

select * from alter2.v1;

select alter2.plus1(41);

-- clean up
drop schema alter2 cascade;
