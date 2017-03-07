--
-- insert with DEFAULT in the target_list
--
create table inserttest (col1 int4, col2 int4 NOT NULL, col3 text default 'testing');
insert into inserttest (col1, col2, col3) values (DEFAULT, DEFAULT, DEFAULT);
insert into inserttest (col2, col3) values (3, DEFAULT);
insert into inserttest (col1, col2, col3) values (DEFAULT, 5, DEFAULT);
insert into inserttest values (DEFAULT, 5, 'test');
insert into inserttest values (DEFAULT, 7);

select * from inserttest;

--
-- insert with similar expression / target_list values (all fail)
--
insert into inserttest (col1, col2, col3) values (DEFAULT, DEFAULT);
insert into inserttest (col1, col2, col3) values (1, 2);
insert into inserttest (col1) values (1, 2);
insert into inserttest (col1) values (DEFAULT, DEFAULT);

select * from inserttest;

--
-- VALUES test
--
insert into inserttest values(10, 20, '40'), (-1, 2, DEFAULT),
    ((select 2), (select i from (values(3)) as foo (i)), 'values are fun!');

select * from inserttest;

--
-- TOASTed value test
--
insert into inserttest values(30, 50, repeat('x', 10000));

select col1, col2, char_length(col3) from inserttest;

drop table inserttest;

--
-- check indirection (field/array assignment), cf bug #14265
--
-- these tests are aware that transformInsertStmt has 3 separate code paths
--

create type insert_test_type as (if1 int, if2 text[]);

create table inserttest (f1 int, f2 int[],
                         f3 insert_test_type, f4 insert_test_type[]);

insert into inserttest (f2[1], f2[2]) values (1,2);
insert into inserttest (f2[1], f2[2]) values (3,4), (5,6);
insert into inserttest (f2[1], f2[2]) select 7,8;
insert into inserttest (f2[1], f2[2]) values (1,default);  -- not supported

insert into inserttest (f3.if1, f3.if2) values (1,array['foo']);
insert into inserttest (f3.if1, f3.if2) values (1,'{foo}'), (2,'{bar}');
insert into inserttest (f3.if1, f3.if2) select 3, '{baz,quux}';
insert into inserttest (f3.if1, f3.if2) values (1,default);  -- not supported

insert into inserttest (f3.if2[1], f3.if2[2]) values ('foo', 'bar');
insert into inserttest (f3.if2[1], f3.if2[2]) values ('foo', 'bar'), ('baz', 'quux');
insert into inserttest (f3.if2[1], f3.if2[2]) select 'bear', 'beer';

insert into inserttest (f4[1].if2[1], f4[1].if2[2]) values ('foo', 'bar');
insert into inserttest (f4[1].if2[1], f4[1].if2[2]) values ('foo', 'bar'), ('baz', 'quux');
insert into inserttest (f4[1].if2[1], f4[1].if2[2]) select 'bear', 'beer';

select * from inserttest;

-- also check reverse-listing
create table inserttest2 (f1 bigint, f2 text);
create rule irule1 as on insert to inserttest2 do also
  insert into inserttest (f3.if2[1], f3.if2[2])
  values (new.f1,new.f2);
create rule irule2 as on insert to inserttest2 do also
  insert into inserttest (f4[1].if1, f4[1].if2[2])
  values (1,'fool'),(new.f1,new.f2);
create rule irule3 as on insert to inserttest2 do also
  insert into inserttest (f4[1].if1, f4[1].if2[2])
  select new.f1, new.f2;
\d+ inserttest2

drop table inserttest2;
drop table inserttest;
drop type insert_test_type;

-- direct partition inserts should check partition bound constraint
create table range_parted (
	a text,
	b int
) partition by range (a, (b+0));
create table part1 partition of range_parted for values from ('a', 1) to ('a', 10);
create table part2 partition of range_parted for values from ('a', 10) to ('a', 20);
create table part3 partition of range_parted for values from ('b', 1) to ('b', 10);
create table part4 partition of range_parted for values from ('b', 10) to ('b', 20);

-- fail
insert into part1 values ('a', 11);
insert into part1 values ('b', 1);
-- ok
insert into part1 values ('a', 1);
-- fail
insert into part4 values ('b', 21);
insert into part4 values ('a', 10);
-- ok
insert into part4 values ('b', 10);

-- fail (partition key a has a NOT NULL constraint)
insert into part1 values (null);
-- fail (expression key (b+0) cannot be null either)
insert into part1 values (1);

create table list_parted (
	a text,
	b int
) partition by list (lower(a));
create table part_aa_bb partition of list_parted FOR VALUES IN ('aa', 'bb');
create table part_cc_dd partition of list_parted FOR VALUES IN ('cc', 'dd');
create table part_null partition of list_parted FOR VALUES IN (null);

-- fail
insert into part_aa_bb values ('cc', 1);
insert into part_aa_bb values ('AAa', 1);
insert into part_aa_bb values (null);
-- ok
insert into part_cc_dd values ('cC', 1);
insert into part_null values (null, 0);

-- check in case of multi-level partitioned table
create table part_ee_ff partition of list_parted for values in ('ee', 'ff') partition by range (b);
create table part_ee_ff1 partition of part_ee_ff for values from (1) to (10);
create table part_ee_ff2 partition of part_ee_ff for values from (10) to (20);

-- fail
insert into part_ee_ff1 values ('EE', 11);
-- fail (even the parent's, ie, part_ee_ff's partition constraint applies)
insert into part_ee_ff1 values ('cc', 1);
-- ok
insert into part_ee_ff1 values ('ff', 1);
insert into part_ee_ff2 values ('ff', 11);

-- Check tuple routing for partitioned tables

-- fail
insert into range_parted values ('a', 0);
-- ok
insert into range_parted values ('a', 1);
insert into range_parted values ('a', 10);
-- fail
insert into range_parted values ('a', 20);
-- ok
insert into range_parted values ('b', 1);
insert into range_parted values ('b', 10);
-- fail (partition key (b+0) is null)
insert into range_parted values ('a');
select tableoid::regclass, * from range_parted;

-- ok
insert into list_parted values (null, 1);
insert into list_parted (a) values ('aA');
-- fail (partition of part_ee_ff not found in both cases)
insert into list_parted values ('EE', 0);
insert into part_ee_ff values ('EE', 0);
-- ok
insert into list_parted values ('EE', 1);
insert into part_ee_ff values ('EE', 10);
select tableoid::regclass, * from list_parted;

-- some more tests to exercise tuple-routing with multi-level partitioning
create table part_gg partition of list_parted for values in ('gg') partition by range (b);
create table part_gg1 partition of part_gg for values from (unbounded) to (1);
create table part_gg2 partition of part_gg for values from (1) to (10) partition by range (b);
create table part_gg2_1 partition of part_gg2 for values from (1) to (5);
create table part_gg2_2 partition of part_gg2 for values from (5) to (10);

create table part_ee_ff3 partition of part_ee_ff for values from (20) to (30) partition by range (b);
create table part_ee_ff3_1 partition of part_ee_ff3 for values from (20) to (25);
create table part_ee_ff3_2 partition of part_ee_ff3 for values from (25) to (30);

truncate list_parted;
insert into list_parted values ('aa'), ('cc');
insert into list_parted select 'Ff', s.a from generate_series(1, 29) s(a);
insert into list_parted select 'gg', s.a from generate_series(1, 9) s(a);
insert into list_parted (b) values (1);
select tableoid::regclass::text, a, min(b) as min_b, max(b) as max_b from list_parted group by 1, 2 order by 1;

-- cleanup
drop table range_parted, list_parted;

-- more tests for certain multi-level partitioning scenarios
create table mlparted (a int, b int) partition by range (a, b);
create table mlparted1 (b int not null, a int not null) partition by range ((b+0));
create table mlparted11 (like mlparted1);
alter table mlparted11 drop a;
alter table mlparted11 add a int;
alter table mlparted11 drop a;
alter table mlparted11 add a int not null;
-- attnum for key attribute 'a' is different in mlparted, mlparted1, and mlparted11
select attrelid::regclass, attname, attnum
from pg_attribute
where attname = 'a'
 and (attrelid = 'mlparted'::regclass
   or attrelid = 'mlparted1'::regclass
   or attrelid = 'mlparted11'::regclass)
order by attrelid::regclass::text;

alter table mlparted1 attach partition mlparted11 for values from (2) to (5);
alter table mlparted attach partition mlparted1 for values from (1, 2) to (1, 10);

-- check that "(1, 2)" is correctly routed to mlparted11.
insert into mlparted values (1, 2);
select tableoid::regclass, * from mlparted;

-- check that proper message is shown after failure to route through mlparted1
insert into mlparted (a, b) values (1, 5);

truncate mlparted;
alter table mlparted add constraint check_b check (b = 3);
-- check that correct input row is shown when constraint check_b fails on mlparted11
-- after "(1, 2)" is routed to it
insert into mlparted values (1, 2);

-- check that inserting into an internal partition successfully results in
-- checking its partition constraint before inserting into the leaf partition
-- selected by tuple-routing
insert into mlparted1 (a, b) values (2, 3);

-- check that RETURNING works correctly with tuple-routing
alter table mlparted drop constraint check_b;
create table mlparted12 partition of mlparted1 for values from (5) to (10);
create table mlparted2 (b int not null, a int not null);
alter table mlparted attach partition mlparted2 for values from (1, 10) to (1, 20);
create table mlparted3 partition of mlparted for values from (1, 20) to (1, 30);
create table mlparted4 (like mlparted);
alter table mlparted4 drop a;
alter table mlparted4 add a int not null;
alter table mlparted attach partition mlparted4 for values from (1, 30) to (1, 40);
with ins (a, b, c) as
  (insert into mlparted (b, a) select s.a, 1 from generate_series(2, 39) s(a) returning tableoid::regclass, *)
  select a, b, min(c), max(c) from ins group by a, b order by 1;

-- check that message shown after failure to find a partition shows the
-- appropriate key description (or none) in various situations
create table key_desc (a int, b int) partition by list ((a+0));
create table key_desc_1 partition of key_desc for values in (1) partition by range (b);

create user someone_else;
grant select (a) on key_desc_1 to someone_else;
grant insert on key_desc to someone_else;

set role someone_else;
-- no key description is shown
insert into key_desc values (1, 1);

reset role;
grant select (b) on key_desc_1 to someone_else;
set role someone_else;
-- key description (b)=(1) is now shown
insert into key_desc values (1, 1);

-- key description is not shown if key contains expression
insert into key_desc values (2, 1);
reset role;
revoke all on key_desc from someone_else;
revoke all on key_desc_1 from someone_else;
drop role someone_else;
drop table key_desc, key_desc_1;
