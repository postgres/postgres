--
-- Test inheritance features
--
CREATE TABLE a (aa TEXT);
CREATE TABLE b (bb TEXT) INHERITS (a);
CREATE TABLE c (cc TEXT) INHERITS (a);
CREATE TABLE d (dd TEXT) INHERITS (b,c,a);

INSERT INTO a(aa) VALUES('aaa');
INSERT INTO a(aa) VALUES('aaaa');
INSERT INTO a(aa) VALUES('aaaaa');
INSERT INTO a(aa) VALUES('aaaaaa');
INSERT INTO a(aa) VALUES('aaaaaaa');
INSERT INTO a(aa) VALUES('aaaaaaaa');

INSERT INTO b(aa) VALUES('bbb');
INSERT INTO b(aa) VALUES('bbbb');
INSERT INTO b(aa) VALUES('bbbbb');
INSERT INTO b(aa) VALUES('bbbbbb');
INSERT INTO b(aa) VALUES('bbbbbbb');
INSERT INTO b(aa) VALUES('bbbbbbbb');

INSERT INTO c(aa) VALUES('ccc');
INSERT INTO c(aa) VALUES('cccc');
INSERT INTO c(aa) VALUES('ccccc');
INSERT INTO c(aa) VALUES('cccccc');
INSERT INTO c(aa) VALUES('ccccccc');
INSERT INTO c(aa) VALUES('cccccccc');

INSERT INTO d(aa) VALUES('ddd');
INSERT INTO d(aa) VALUES('dddd');
INSERT INTO d(aa) VALUES('ddddd');
INSERT INTO d(aa) VALUES('dddddd');
INSERT INTO d(aa) VALUES('ddddddd');
INSERT INTO d(aa) VALUES('dddddddd');

SELECT relname, a.* FROM a, pg_class where a.tableoid = pg_class.oid;
SELECT relname, b.* FROM b, pg_class where b.tableoid = pg_class.oid;
SELECT relname, c.* FROM c, pg_class where c.tableoid = pg_class.oid;
SELECT relname, d.* FROM d, pg_class where d.tableoid = pg_class.oid;
SELECT relname, a.* FROM ONLY a, pg_class where a.tableoid = pg_class.oid;
SELECT relname, b.* FROM ONLY b, pg_class where b.tableoid = pg_class.oid;
SELECT relname, c.* FROM ONLY c, pg_class where c.tableoid = pg_class.oid;
SELECT relname, d.* FROM ONLY d, pg_class where d.tableoid = pg_class.oid;

UPDATE a SET aa='zzzz' WHERE aa='aaaa';
UPDATE ONLY a SET aa='zzzzz' WHERE aa='aaaaa';
UPDATE b SET aa='zzz' WHERE aa='aaa';
UPDATE ONLY b SET aa='zzz' WHERE aa='aaa';
UPDATE a SET aa='zzzzzz' WHERE aa LIKE 'aaa%';

SELECT relname, a.* FROM a, pg_class where a.tableoid = pg_class.oid;
SELECT relname, b.* FROM b, pg_class where b.tableoid = pg_class.oid;
SELECT relname, c.* FROM c, pg_class where c.tableoid = pg_class.oid;
SELECT relname, d.* FROM d, pg_class where d.tableoid = pg_class.oid;
SELECT relname, a.* FROM ONLY a, pg_class where a.tableoid = pg_class.oid;
SELECT relname, b.* FROM ONLY b, pg_class where b.tableoid = pg_class.oid;
SELECT relname, c.* FROM ONLY c, pg_class where c.tableoid = pg_class.oid;
SELECT relname, d.* FROM ONLY d, pg_class where d.tableoid = pg_class.oid;

UPDATE b SET aa='new';

SELECT relname, a.* FROM a, pg_class where a.tableoid = pg_class.oid;
SELECT relname, b.* FROM b, pg_class where b.tableoid = pg_class.oid;
SELECT relname, c.* FROM c, pg_class where c.tableoid = pg_class.oid;
SELECT relname, d.* FROM d, pg_class where d.tableoid = pg_class.oid;
SELECT relname, a.* FROM ONLY a, pg_class where a.tableoid = pg_class.oid;
SELECT relname, b.* FROM ONLY b, pg_class where b.tableoid = pg_class.oid;
SELECT relname, c.* FROM ONLY c, pg_class where c.tableoid = pg_class.oid;
SELECT relname, d.* FROM ONLY d, pg_class where d.tableoid = pg_class.oid;

UPDATE a SET aa='new';

DELETE FROM ONLY c WHERE aa='new';

SELECT relname, a.* FROM a, pg_class where a.tableoid = pg_class.oid;
SELECT relname, b.* FROM b, pg_class where b.tableoid = pg_class.oid;
SELECT relname, c.* FROM c, pg_class where c.tableoid = pg_class.oid;
SELECT relname, d.* FROM d, pg_class where d.tableoid = pg_class.oid;
SELECT relname, a.* FROM ONLY a, pg_class where a.tableoid = pg_class.oid;
SELECT relname, b.* FROM ONLY b, pg_class where b.tableoid = pg_class.oid;
SELECT relname, c.* FROM ONLY c, pg_class where c.tableoid = pg_class.oid;
SELECT relname, d.* FROM ONLY d, pg_class where d.tableoid = pg_class.oid;

DELETE FROM a;

SELECT relname, a.* FROM a, pg_class where a.tableoid = pg_class.oid;
SELECT relname, b.* FROM b, pg_class where b.tableoid = pg_class.oid;
SELECT relname, c.* FROM c, pg_class where c.tableoid = pg_class.oid;
SELECT relname, d.* FROM d, pg_class where d.tableoid = pg_class.oid;
SELECT relname, a.* FROM ONLY a, pg_class where a.tableoid = pg_class.oid;
SELECT relname, b.* FROM ONLY b, pg_class where b.tableoid = pg_class.oid;
SELECT relname, c.* FROM ONLY c, pg_class where c.tableoid = pg_class.oid;
SELECT relname, d.* FROM ONLY d, pg_class where d.tableoid = pg_class.oid;

-- Confirm PRIMARY KEY adds NOT NULL constraint to child table
CREATE TEMP TABLE z (b TEXT, PRIMARY KEY(aa, b)) inherits (a);
INSERT INTO z VALUES (NULL, 'text'); -- should fail

-- Check UPDATE with inherited target and an inherited source table
create temp table foo(f1 int, f2 int);
create temp table foo2(f3 int) inherits (foo);
create temp table bar(f1 int, f2 int);
create temp table bar2(f3 int) inherits (bar);

insert into foo values(1,1);
insert into foo values(3,3);
insert into foo2 values(2,2,2);
insert into foo2 values(3,3,3);
insert into bar values(1,1);
insert into bar values(2,2);
insert into bar values(3,3);
insert into bar values(4,4);
insert into bar2 values(1,1,1);
insert into bar2 values(2,2,2);
insert into bar2 values(3,3,3);
insert into bar2 values(4,4,4);

update bar set f2 = f2 + 100 where f1 in (select f1 from foo);

SELECT relname, bar.* FROM bar, pg_class where bar.tableoid = pg_class.oid
order by 1,2;

/* Test multiple inheritance of column defaults */

CREATE TABLE firstparent (tomorrow date default now()::date + 1);
CREATE TABLE secondparent (tomorrow date default  now() :: date  +  1);
CREATE TABLE jointchild () INHERITS (firstparent, secondparent);  -- ok
CREATE TABLE thirdparent (tomorrow date default now()::date - 1);
CREATE TABLE otherchild () INHERITS (firstparent, thirdparent);  -- not ok
CREATE TABLE otherchild (tomorrow date default now())
  INHERITS (firstparent, thirdparent);  -- ok, child resolves ambiguous default

DROP TABLE firstparent, secondparent, jointchild, thirdparent, otherchild;

/* Test inheritance of structure (LIKE) */
CREATE TABLE inhx (xx text DEFAULT 'text');

/*
 * Test double inheritance
 *
 * Ensure that defaults are NOT included unless
 * INCLUDING DEFAULTS is specified 
 */
CREATE TABLE inhe (ee text, LIKE inhx) inherits (b);
INSERT INTO inhe VALUES ('ee-col1', 'ee-col2', DEFAULT, 'ee-col4');
SELECT * FROM inhe; /* Columns aa, bb, xx value NULL, ee */
SELECT * FROM inhx; /* Empty set since LIKE inherits structure only */
SELECT * FROM b; /* Has ee entry */
SELECT * FROM a; /* Has ee entry */

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

-- Test changing the type of inherited columns
insert into d values('test','one','two','three');
alter table a alter column aa type integer using bit_length(aa);
select * from d;

-- Tests for casting between the rowtypes of parent and child
-- tables. See the pgsql-hackers thread beginning Dec. 4/04
create table base (i integer);
create table derived () inherits (base);
insert into derived (i) values (0);
select derived::base from derived;
drop table derived;
drop table base;

create table p1(ff1 int);
create table p2(f1 text);
create function p2text(p2) returns text as 'select $1.f1' language sql;
create table c1(f3 int) inherits(p1,p2);
insert into c1 values(123456789, 'hi', 42);
select p2text(c1.*) from c1;
drop function p2text(p2);
drop table c1;
drop table p2;
drop table p1;

CREATE TABLE ac (aa TEXT);
alter table ac add constraint ac_check check (aa is not null);
CREATE TABLE bc (bb TEXT) INHERITS (ac);
select pc.relname, pgc.conname, pgc.contype, pgc.conislocal, pgc.coninhcount, pgc.consrc from pg_class as pc inner join pg_constraint as pgc on (pgc.conrelid = pc.oid) where pc.relname in ('ac', 'bc') order by 1,2;

insert into ac (aa) values (NULL);
insert into bc (aa) values (NULL);

alter table bc drop constraint ac_check;  -- fail, disallowed
alter table ac drop constraint ac_check;
select pc.relname, pgc.conname, pgc.contype, pgc.conislocal, pgc.coninhcount, pgc.consrc from pg_class as pc inner join pg_constraint as pgc on (pgc.conrelid = pc.oid) where pc.relname in ('ac', 'bc') order by 1,2;

-- try the unnamed-constraint case
alter table ac add check (aa is not null);
select pc.relname, pgc.conname, pgc.contype, pgc.conislocal, pgc.coninhcount, pgc.consrc from pg_class as pc inner join pg_constraint as pgc on (pgc.conrelid = pc.oid) where pc.relname in ('ac', 'bc') order by 1,2;

insert into ac (aa) values (NULL);
insert into bc (aa) values (NULL);

alter table bc drop constraint ac_aa_check;  -- fail, disallowed
alter table ac drop constraint ac_aa_check;
select pc.relname, pgc.conname, pgc.contype, pgc.conislocal, pgc.coninhcount, pgc.consrc from pg_class as pc inner join pg_constraint as pgc on (pgc.conrelid = pc.oid) where pc.relname in ('ac', 'bc') order by 1,2;

alter table ac add constraint ac_check check (aa is not null);
alter table bc no inherit ac;
select pc.relname, pgc.conname, pgc.contype, pgc.conislocal, pgc.coninhcount, pgc.consrc from pg_class as pc inner join pg_constraint as pgc on (pgc.conrelid = pc.oid) where pc.relname in ('ac', 'bc') order by 1,2;
alter table bc drop constraint ac_check;
select pc.relname, pgc.conname, pgc.contype, pgc.conislocal, pgc.coninhcount, pgc.consrc from pg_class as pc inner join pg_constraint as pgc on (pgc.conrelid = pc.oid) where pc.relname in ('ac', 'bc') order by 1,2;
alter table ac drop constraint ac_check;
select pc.relname, pgc.conname, pgc.contype, pgc.conislocal, pgc.coninhcount, pgc.consrc from pg_class as pc inner join pg_constraint as pgc on (pgc.conrelid = pc.oid) where pc.relname in ('ac', 'bc') order by 1,2;

drop table bc;
drop table ac;

create table ac (a int constraint check_a check (a <> 0));
create table bc (a int constraint check_a check (a <> 0), b int constraint check_b check (b <> 0)) inherits (ac);
select pc.relname, pgc.conname, pgc.contype, pgc.conislocal, pgc.coninhcount, pgc.consrc from pg_class as pc inner join pg_constraint as pgc on (pgc.conrelid = pc.oid) where pc.relname in ('ac', 'bc') order by 1,2;

drop table bc;
drop table ac;

create table ac (a int constraint check_a check (a <> 0));
create table bc (b int constraint check_b check (b <> 0));
create table cc (c int constraint check_c check (c <> 0)) inherits (ac, bc);
select pc.relname, pgc.conname, pgc.contype, pgc.conislocal, pgc.coninhcount, pgc.consrc from pg_class as pc inner join pg_constraint as pgc on (pgc.conrelid = pc.oid) where pc.relname in ('ac', 'bc', 'cc') order by 1,2;

alter table cc no inherit bc;
select pc.relname, pgc.conname, pgc.contype, pgc.conislocal, pgc.coninhcount, pgc.consrc from pg_class as pc inner join pg_constraint as pgc on (pgc.conrelid = pc.oid) where pc.relname in ('ac', 'bc', 'cc') order by 1,2;

drop table cc;
drop table bc;
drop table ac;

create table p1(f1 int);
create table p2(f2 int);
create table c1(f3 int) inherits(p1,p2);
insert into c1 values(1,-1,2);
alter table p2 add constraint cc check (f2>0);  -- fail
alter table p2 add check (f2>0);  -- check it without a name, too
delete from c1;
insert into c1 values(1,1,2);
alter table p2 add check (f2>0);
insert into c1 values(1,-1,2);  -- fail
create table c2(f3 int) inherits(p1,p2);
\d c2
create table c3 (f4 int) inherits(c1,c2);
\d c3
drop table p1 cascade;
drop table p2 cascade;

create table pp1 (f1 int);
create table cc1 (f2 text, f3 int) inherits (pp1);
alter table pp1 add column a1 int check (a1 > 0);
\d cc1
create table cc2(f4 float) inherits(pp1,cc1);
\d cc2
alter table pp1 add column a2 int check (a2 > 0);
\d cc2
drop table pp1 cascade;

-- including storage and comments
CREATE TABLE t1 (a text CHECK (length(a) > 2) PRIMARY KEY, b text);
CREATE INDEX t1_b_key ON t1 (b);
CREATE INDEX t1_fnidx ON t1 ((a || b));
COMMENT ON COLUMN t1.a IS 'A';
COMMENT ON COLUMN t1.b IS 'B';
COMMENT ON CONSTRAINT t1_a_check ON t1 IS 't1_a_check';
COMMENT ON INDEX t1_pkey IS 'index pkey';
COMMENT ON INDEX t1_b_key IS 'index b_key';
ALTER TABLE t1 ALTER COLUMN a SET STORAGE MAIN;

CREATE TABLE t2 (c text);
ALTER TABLE t2 ALTER COLUMN c SET STORAGE EXTERNAL;
COMMENT ON COLUMN t2.c IS 'C';

CREATE TABLE t3 (a text CHECK (length(a) < 5), c text);
ALTER TABLE t3 ALTER COLUMN c SET STORAGE EXTERNAL;
ALTER TABLE t3 ALTER COLUMN a SET STORAGE MAIN;
COMMENT ON COLUMN t3.a IS 'A3';
COMMENT ON COLUMN t3.c IS 'C';
COMMENT ON CONSTRAINT t3_a_check ON t3 IS 't3_a_check';

CREATE TABLE t4 (a text, c text);
ALTER TABLE t4 ALTER COLUMN c SET STORAGE EXTERNAL;

CREATE TABLE t12_storage (LIKE t1 INCLUDING STORAGE, LIKE t2 INCLUDING STORAGE);
\d+ t12_storage
CREATE TABLE t12_comments (LIKE t1 INCLUDING COMMENTS, LIKE t2 INCLUDING COMMENTS);
\d+ t12_comments
CREATE TABLE t1_inh (LIKE t1 INCLUDING CONSTRAINTS INCLUDING COMMENTS) INHERITS (t1);
\d+ t1_inh
SELECT description FROM pg_description, pg_constraint c WHERE classoid = 'pg_constraint'::regclass AND objoid = c.oid AND c.conrelid = 't1_inh'::regclass;
CREATE TABLE t13_inh () INHERITS (t1, t3);
\d+ t13_inh
CREATE TABLE t13_like (LIKE t3 INCLUDING CONSTRAINTS INCLUDING COMMENTS INCLUDING STORAGE) INHERITS (t1);
\d+ t13_like
SELECT description FROM pg_description, pg_constraint c WHERE classoid = 'pg_constraint'::regclass AND objoid = c.oid AND c.conrelid = 't13_like'::regclass;

CREATE TABLE t_all (LIKE t1 INCLUDING ALL);
\d+ t_all
SELECT c.relname, objsubid, description FROM pg_description, pg_index i, pg_class c WHERE classoid = 'pg_class'::regclass AND objoid = i.indexrelid AND c.oid = i.indexrelid AND i.indrelid = 't_all'::regclass ORDER BY c.relname, objsubid;

CREATE TABLE inh_error1 () INHERITS (t1, t4);
CREATE TABLE inh_error2 (LIKE t4 INCLUDING STORAGE) INHERITS (t1);

DROP TABLE t1, t2, t3, t4, t12_storage, t12_comments, t1_inh, t13_inh, t13_like, t_all;
