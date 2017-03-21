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

select tableoid::regclass::text as relname, bar.* from bar order by 1,2;

-- Check UPDATE with inherited target and an appendrel subquery
update bar set f2 = f2 + 100
from
  ( select f1 from foo union all select f1+3 from foo ) ss
where bar.f1 = ss.f1;

select tableoid::regclass::text as relname, bar.* from bar order by 1,2;

-- Check UPDATE with *partitioned* inherited target and an appendrel subquery
create table some_tab (a int);
insert into some_tab values (0);
create table some_tab_child () inherits (some_tab);
insert into some_tab_child values (1);
create table parted_tab (a int, b char) partition by list (a);
create table parted_tab_part1 partition of parted_tab for values in (1);
create table parted_tab_part2 partition of parted_tab for values in (2);
create table parted_tab_part3 partition of parted_tab for values in (3);
insert into parted_tab values (1, 'a'), (2, 'a'), (3, 'a');

update parted_tab set b = 'b'
from
  (select a from some_tab union all select a+1 from some_tab) ss (a)
where parted_tab.a = ss.a;
select tableoid::regclass::text as relname, parted_tab.* from parted_tab order by 1,2;

truncate parted_tab;
insert into parted_tab values (1, 'a'), (2, 'a'), (3, 'a');
update parted_tab set b = 'b'
from
  (select 0 from parted_tab union all select 1 from parted_tab) ss (a)
where parted_tab.a = ss.a;
select tableoid::regclass::text as relname, parted_tab.* from parted_tab order by 1,2;

drop table parted_tab;
drop table some_tab cascade;

/* Test multiple inheritance of column defaults */

CREATE TABLE firstparent (tomorrow date default now()::date + 1);
CREATE TABLE secondparent (tomorrow date default  now() :: date  +  1);
CREATE TABLE jointchild () INHERITS (firstparent, secondparent);  -- ok
CREATE TABLE thirdparent (tomorrow date default now()::date - 1);
CREATE TABLE otherchild () INHERITS (firstparent, thirdparent);  -- not ok
CREATE TABLE otherchild (tomorrow date default now())
  INHERITS (firstparent, thirdparent);  -- ok, child resolves ambiguous default

DROP TABLE firstparent, secondparent, jointchild, thirdparent, otherchild;

-- Test changing the type of inherited columns
insert into d values('test','one','two','three');
alter table a alter column aa type integer using bit_length(aa);
select * from d;

-- check that oid column is handled properly during alter table inherit
create table oid_parent (a int) with oids;

create table oid_child () inherits (oid_parent);
select attinhcount, attislocal from pg_attribute
  where attrelid = 'oid_child'::regclass and attname = 'oid';
drop table oid_child;

create table oid_child (a int) without oids;
alter table oid_child inherit oid_parent;  -- fail
alter table oid_child set with oids;
select attinhcount, attislocal from pg_attribute
  where attrelid = 'oid_child'::regclass and attname = 'oid';
alter table oid_child inherit oid_parent;
select attinhcount, attislocal from pg_attribute
  where attrelid = 'oid_child'::regclass and attname = 'oid';
alter table oid_child set without oids;  -- fail
alter table oid_parent set without oids;
select attinhcount, attislocal from pg_attribute
  where attrelid = 'oid_child'::regclass and attname = 'oid';
alter table oid_child set without oids;
select attinhcount, attislocal from pg_attribute
  where attrelid = 'oid_child'::regclass and attname = 'oid';

drop table oid_parent cascade;

-- Test non-inheritable parent constraints
create table p1(ff1 int);
alter table p1 add constraint p1chk check (ff1 > 0) no inherit;
alter table p1 add constraint p2chk check (ff1 > 10);
-- connoinherit should be true for NO INHERIT constraint
select pc.relname, pgc.conname, pgc.contype, pgc.conislocal, pgc.coninhcount, pgc.connoinherit from pg_class as pc inner join pg_constraint as pgc on (pgc.conrelid = pc.oid) where pc.relname = 'p1' order by 1,2;

-- Test that child does not inherit NO INHERIT constraints
create table c1 () inherits (p1);
\d p1
\d c1

-- Test that child does not override inheritable constraints of the parent
create table c2 (constraint p2chk check (ff1 > 10) no inherit) inherits (p1);	--fails

drop table p1 cascade;

-- Tests for casting between the rowtypes of parent and child
-- tables. See the pgsql-hackers thread beginning Dec. 4/04
create table base (i integer);
create table derived () inherits (base);
insert into derived (i) values (0);
select derived::base from derived;
select NULL::derived::base;
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

-- Test for renaming in simple multiple inheritance
CREATE TABLE inht1 (a int, b int);
CREATE TABLE inhs1 (b int, c int);
CREATE TABLE inhts (d int) INHERITS (inht1, inhs1);

ALTER TABLE inht1 RENAME a TO aa;
ALTER TABLE inht1 RENAME b TO bb;                -- to be failed
ALTER TABLE inhts RENAME aa TO aaa;      -- to be failed
ALTER TABLE inhts RENAME d TO dd;
\d+ inhts

DROP TABLE inhts;

-- Test for renaming in diamond inheritance
CREATE TABLE inht2 (x int) INHERITS (inht1);
CREATE TABLE inht3 (y int) INHERITS (inht1);
CREATE TABLE inht4 (z int) INHERITS (inht2, inht3);

ALTER TABLE inht1 RENAME aa TO aaa;
\d+ inht4

CREATE TABLE inhts (d int) INHERITS (inht2, inhs1);
ALTER TABLE inht1 RENAME aaa TO aaaa;
ALTER TABLE inht1 RENAME b TO bb;                -- to be failed
\d+ inhts

WITH RECURSIVE r AS (
  SELECT 'inht1'::regclass AS inhrelid
UNION ALL
  SELECT c.inhrelid FROM pg_inherits c, r WHERE r.inhrelid = c.inhparent
)
SELECT a.attrelid::regclass, a.attname, a.attinhcount, e.expected
  FROM (SELECT inhrelid, count(*) AS expected FROM pg_inherits
        WHERE inhparent IN (SELECT inhrelid FROM r) GROUP BY inhrelid) e
  JOIN pg_attribute a ON e.inhrelid = a.attrelid WHERE NOT attislocal
  ORDER BY a.attrelid::regclass::name, a.attnum;

DROP TABLE inht1, inhs1 CASCADE;


-- Test non-inheritable indices [UNIQUE, EXCLUDE] constraints
CREATE TABLE test_constraints (id int, val1 varchar, val2 int, UNIQUE(val1, val2));
CREATE TABLE test_constraints_inh () INHERITS (test_constraints);
\d+ test_constraints
ALTER TABLE ONLY test_constraints DROP CONSTRAINT test_constraints_val1_val2_key;
\d+ test_constraints
\d+ test_constraints_inh
DROP TABLE test_constraints_inh;
DROP TABLE test_constraints;

CREATE TABLE test_ex_constraints (
    c circle,
    EXCLUDE USING gist (c WITH &&)
);
CREATE TABLE test_ex_constraints_inh () INHERITS (test_ex_constraints);
\d+ test_ex_constraints
ALTER TABLE test_ex_constraints DROP CONSTRAINT test_ex_constraints_c_excl;
\d+ test_ex_constraints
\d+ test_ex_constraints_inh
DROP TABLE test_ex_constraints_inh;
DROP TABLE test_ex_constraints;

-- Test non-inheritable foreign key constraints
CREATE TABLE test_primary_constraints(id int PRIMARY KEY);
CREATE TABLE test_foreign_constraints(id1 int REFERENCES test_primary_constraints(id));
CREATE TABLE test_foreign_constraints_inh () INHERITS (test_foreign_constraints);
\d+ test_primary_constraints
\d+ test_foreign_constraints
ALTER TABLE test_foreign_constraints DROP CONSTRAINT test_foreign_constraints_id1_fkey;
\d+ test_foreign_constraints
\d+ test_foreign_constraints_inh
DROP TABLE test_foreign_constraints_inh;
DROP TABLE test_foreign_constraints;
DROP TABLE test_primary_constraints;

-- Test that parent and child CHECK constraints can be created in either order
create table p1(f1 int);
create table p1_c1() inherits(p1);

alter table p1 add constraint inh_check_constraint1 check (f1 > 0);
alter table p1_c1 add constraint inh_check_constraint1 check (f1 > 0);

alter table p1_c1 add constraint inh_check_constraint2 check (f1 < 10);
alter table p1 add constraint inh_check_constraint2 check (f1 < 10);

select conrelid::regclass::text as relname, conname, conislocal, coninhcount
from pg_constraint where conname like 'inh\_check\_constraint%'
order by 1, 2;

drop table p1 cascade;

-- Test that a valid child can have not-valid parent, but not vice versa
create table invalid_check_con(f1 int);
create table invalid_check_con_child() inherits(invalid_check_con);

alter table invalid_check_con_child add constraint inh_check_constraint check(f1 > 0) not valid;
alter table invalid_check_con add constraint inh_check_constraint check(f1 > 0); -- fail
alter table invalid_check_con_child drop constraint inh_check_constraint;

insert into invalid_check_con values(0);

alter table invalid_check_con_child add constraint inh_check_constraint check(f1 > 0);
alter table invalid_check_con add constraint inh_check_constraint check(f1 > 0) not valid;

insert into invalid_check_con values(0); -- fail
insert into invalid_check_con_child values(0); -- fail

select conrelid::regclass::text as relname, conname,
       convalidated, conislocal, coninhcount, connoinherit
from pg_constraint where conname like 'inh\_check\_constraint%'
order by 1, 2;

-- We don't drop the invalid_check_con* tables, to test dump/reload with

--
-- Test parameterized append plans for inheritance trees
--

create temp table patest0 (id, x) as
  select x, x from generate_series(0,1000) x;
create temp table patest1() inherits (patest0);
insert into patest1
  select x, x from generate_series(0,1000) x;
create temp table patest2() inherits (patest0);
insert into patest2
  select x, x from generate_series(0,1000) x;
create index patest0i on patest0(id);
create index patest1i on patest1(id);
create index patest2i on patest2(id);
analyze patest0;
analyze patest1;
analyze patest2;

explain (costs off)
select * from patest0 join (select f1 from int4_tbl limit 1) ss on id = f1;
select * from patest0 join (select f1 from int4_tbl limit 1) ss on id = f1;

drop index patest2i;

explain (costs off)
select * from patest0 join (select f1 from int4_tbl limit 1) ss on id = f1;
select * from patest0 join (select f1 from int4_tbl limit 1) ss on id = f1;

drop table patest0 cascade;

--
-- Test merge-append plans for inheritance trees
--

create table matest0 (id serial primary key, name text);
create table matest1 (id integer primary key) inherits (matest0);
create table matest2 (id integer primary key) inherits (matest0);
create table matest3 (id integer primary key) inherits (matest0);

create index matest0i on matest0 ((1-id));
create index matest1i on matest1 ((1-id));
-- create index matest2i on matest2 ((1-id));  -- intentionally missing
create index matest3i on matest3 ((1-id));

insert into matest1 (name) values ('Test 1');
insert into matest1 (name) values ('Test 2');
insert into matest2 (name) values ('Test 3');
insert into matest2 (name) values ('Test 4');
insert into matest3 (name) values ('Test 5');
insert into matest3 (name) values ('Test 6');

set enable_indexscan = off;  -- force use of seqscan/sort, so no merge
explain (verbose, costs off) select * from matest0 order by 1-id;
select * from matest0 order by 1-id;
explain (verbose, costs off) select min(1-id) from matest0;
select min(1-id) from matest0;
reset enable_indexscan;

set enable_seqscan = off;  -- plan with fewest seqscans should be merge
explain (verbose, costs off) select * from matest0 order by 1-id;
select * from matest0 order by 1-id;
explain (verbose, costs off) select min(1-id) from matest0;
select min(1-id) from matest0;
reset enable_seqscan;

drop table matest0 cascade;

--
-- Check that use of an index with an extraneous column doesn't produce
-- a plan with extraneous sorting
--

create table matest0 (a int, b int, c int, d int);
create table matest1 () inherits(matest0);
create index matest0i on matest0 (b, c);
create index matest1i on matest1 (b, c);

set enable_nestloop = off;  -- we want a plan with two MergeAppends

explain (costs off)
select t1.* from matest0 t1, matest0 t2
where t1.b = t2.b and t2.c = t2.d
order by t1.b limit 10;

reset enable_nestloop;

drop table matest0 cascade;

--
-- Test merge-append for UNION ALL append relations
--

set enable_seqscan = off;
set enable_indexscan = on;
set enable_bitmapscan = off;

-- Check handling of duplicated, constant, or volatile targetlist items
explain (costs off)
SELECT thousand, tenthous FROM tenk1
UNION ALL
SELECT thousand, thousand FROM tenk1
ORDER BY thousand, tenthous;

explain (costs off)
SELECT thousand, tenthous, thousand+tenthous AS x FROM tenk1
UNION ALL
SELECT 42, 42, hundred FROM tenk1
ORDER BY thousand, tenthous;

explain (costs off)
SELECT thousand, tenthous FROM tenk1
UNION ALL
SELECT thousand, random()::integer FROM tenk1
ORDER BY thousand, tenthous;

-- Check min/max aggregate optimization
explain (costs off)
SELECT min(x) FROM
  (SELECT unique1 AS x FROM tenk1 a
   UNION ALL
   SELECT unique2 AS x FROM tenk1 b) s;

explain (costs off)
SELECT min(y) FROM
  (SELECT unique1 AS x, unique1 AS y FROM tenk1 a
   UNION ALL
   SELECT unique2 AS x, unique2 AS y FROM tenk1 b) s;

-- XXX planner doesn't recognize that index on unique2 is sufficiently sorted
explain (costs off)
SELECT x, y FROM
  (SELECT thousand AS x, tenthous AS y FROM tenk1 a
   UNION ALL
   SELECT unique2 AS x, unique2 AS y FROM tenk1 b) s
ORDER BY x, y;

-- exercise rescan code path via a repeatedly-evaluated subquery
explain (costs off)
SELECT
    ARRAY(SELECT f.i FROM (
        (SELECT d + g.i FROM generate_series(4, 30, 3) d ORDER BY 1)
        UNION ALL
        (SELECT d + g.i FROM generate_series(0, 30, 5) d ORDER BY 1)
    ) f(i)
    ORDER BY f.i LIMIT 10)
FROM generate_series(1, 3) g(i);

SELECT
    ARRAY(SELECT f.i FROM (
        (SELECT d + g.i FROM generate_series(4, 30, 3) d ORDER BY 1)
        UNION ALL
        (SELECT d + g.i FROM generate_series(0, 30, 5) d ORDER BY 1)
    ) f(i)
    ORDER BY f.i LIMIT 10)
FROM generate_series(1, 3) g(i);

reset enable_seqscan;
reset enable_indexscan;
reset enable_bitmapscan;

--
-- Check that constraint exclusion works correctly with partitions using
-- implicit constraints generated from the partition bound information.
--
create table list_parted (
	a	varchar
) partition by list (a);
create table part_ab_cd partition of list_parted for values in ('ab', 'cd');
create table part_ef_gh partition of list_parted for values in ('ef', 'gh');
create table part_null_xy partition of list_parted for values in (null, 'xy');

explain (costs off) select * from list_parted;
explain (costs off) select * from list_parted where a is null;
explain (costs off) select * from list_parted where a is not null;
explain (costs off) select * from list_parted where a in ('ab', 'cd', 'ef');
explain (costs off) select * from list_parted where a = 'ab' or a in (null, 'cd');
explain (costs off) select * from list_parted where a = 'ab';

create table range_list_parted (
	a	int,
	b	char(2)
) partition by range (a);
create table part_1_10 partition of range_list_parted for values from (1) to (10) partition by list (b);
create table part_1_10_ab partition of part_1_10 for values in ('ab');
create table part_1_10_cd partition of part_1_10 for values in ('cd');
create table part_10_20 partition of range_list_parted for values from (10) to (20) partition by list (b);
create table part_10_20_ab partition of part_10_20 for values in ('ab');
create table part_10_20_cd partition of part_10_20 for values in ('cd');
create table part_21_30 partition of range_list_parted for values from (21) to (30) partition by list (b);
create table part_21_30_ab partition of part_21_30 for values in ('ab');
create table part_21_30_cd partition of part_21_30 for values in ('cd');
create table part_40_inf partition of range_list_parted for values from (40) to (unbounded) partition by list (b);
create table part_40_inf_ab partition of part_40_inf for values in ('ab');
create table part_40_inf_cd partition of part_40_inf for values in ('cd');
create table part_40_inf_null partition of part_40_inf for values in (null);

explain (costs off) select * from range_list_parted;
explain (costs off) select * from range_list_parted where a = 5;
explain (costs off) select * from range_list_parted where b = 'ab';
explain (costs off) select * from range_list_parted where a between 3 and 23 and b in ('ab');

/* Should select no rows because range partition key cannot be null */
explain (costs off) select * from range_list_parted where a is null;

/* Should only select rows from the null-accepting partition */
explain (costs off) select * from range_list_parted where b is null;
explain (costs off) select * from range_list_parted where a is not null and a < 67;
explain (costs off) select * from range_list_parted where a >= 30;

drop table list_parted;
drop table range_list_parted;
