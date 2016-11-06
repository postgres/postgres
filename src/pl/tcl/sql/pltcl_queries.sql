-- suppress CONTEXT so that function OIDs aren't in output
\set VERBOSITY terse

insert into T_pkey1 values (1, 'key1-1', 'test key');
insert into T_pkey1 values (1, 'key1-2', 'test key');
insert into T_pkey1 values (1, 'key1-3', 'test key');
insert into T_pkey1 values (2, 'key2-1', 'test key');
insert into T_pkey1 values (2, 'key2-2', 'test key');
insert into T_pkey1 values (2, 'key2-3', 'test key');

insert into T_pkey2 values (1, 'key1-1', 'test key');
insert into T_pkey2 values (1, 'key1-2', 'test key');
insert into T_pkey2 values (1, 'key1-3', 'test key');
insert into T_pkey2 values (2, 'key2-1', 'test key');
insert into T_pkey2 values (2, 'key2-2', 'test key');
insert into T_pkey2 values (2, 'key2-3', 'test key');

select * from T_pkey1;

-- key2 in T_pkey2 should have upper case only
select * from T_pkey2;

insert into T_pkey1 values (1, 'KEY1-3', 'should work');

-- Due to the upper case translation in trigger this must fail
insert into T_pkey2 values (1, 'KEY1-3', 'should fail');

insert into T_dta1 values ('trec 1', 1, 'key1-1');
insert into T_dta1 values ('trec 2', 1, 'key1-2');
insert into T_dta1 values ('trec 3', 1, 'key1-3');

-- Must fail due to unknown key in T_pkey1
insert into T_dta1 values ('trec 4', 1, 'key1-4');

insert into T_dta2 values ('trec 1', 1, 'KEY1-1');
insert into T_dta2 values ('trec 2', 1, 'KEY1-2');
insert into T_dta2 values ('trec 3', 1, 'KEY1-3');

-- Must fail due to unknown key in T_pkey2
insert into T_dta2 values ('trec 4', 1, 'KEY1-4');

select * from T_dta1;

select * from T_dta2;

update T_pkey1 set key2 = 'key2-9' where key1 = 2 and key2 = 'key2-1';
update T_pkey1 set key2 = 'key1-9' where key1 = 1 and key2 = 'key1-1';
delete from T_pkey1 where key1 = 2 and key2 = 'key2-2';
delete from T_pkey1 where key1 = 1 and key2 = 'key1-2';

update T_pkey2 set key2 = 'KEY2-9' where key1 = 2 and key2 = 'KEY2-1';
update T_pkey2 set key2 = 'KEY1-9' where key1 = 1 and key2 = 'KEY1-1';
delete from T_pkey2 where key1 = 2 and key2 = 'KEY2-2';
delete from T_pkey2 where key1 = 1 and key2 = 'KEY1-2';

select * from T_pkey1;
select * from T_pkey2;
select * from T_dta1;
select * from T_dta2;

select tcl_avg(key1) from T_pkey1;
select tcl_sum(key1) from T_pkey1;
select tcl_avg(key1) from T_pkey2;
select tcl_sum(key1) from T_pkey2;

-- The following should return NULL instead of 0
select tcl_avg(key1) from T_pkey1 where key1 = 99;
select tcl_sum(key1) from T_pkey1 where key1 = 99;

select 1 @< 2;
select 100 @< 4;

select * from T_pkey1 order by key1 using @<, key2 collate "C";
select * from T_pkey2 order by key1 using @<, key2 collate "C";

-- show dump of trigger data
insert into trigger_test values(1,'insert');

insert into trigger_test_view values(2,'insert');
update trigger_test_view set v = 'update' where i=1;
delete from trigger_test_view;

update trigger_test set v = 'update' where i = 1;
delete from trigger_test;

-- Test composite-type arguments
select tcl_composite_arg_ref1(row('tkey', 42, 'ref2'));
select tcl_composite_arg_ref2(row('tkey', 42, 'ref2'));

-- Test argisnull primitive
select tcl_argisnull('foo');
select tcl_argisnull('');
select tcl_argisnull(null);

-- Test spi_lastoid primitive
create temp table t1 (f1 int);
select tcl_lastoid('t1');
create temp table t2 (f1 int) with oids;
select tcl_lastoid('t2') > 0;

-- test some error cases
CREATE FUNCTION tcl_error(OUT a int, OUT b int) AS $$return {$$ LANGUAGE pltcl;
SELECT tcl_error();

CREATE FUNCTION bad_record(OUT a text, OUT b text) AS $$return [list a]$$ LANGUAGE pltcl;
SELECT bad_record();

CREATE FUNCTION bad_field(OUT a text, OUT b text) AS $$return [list a 1 b 2 cow 3]$$ LANGUAGE pltcl;
SELECT bad_field();

-- test compound return
select * from tcl_test_cube_squared(5);

-- test SRF
select * from tcl_test_squared_rows(0,5);

select * from tcl_test_sequence(0,5) as a;

select 1, tcl_test_sequence(0,5);

CREATE FUNCTION non_srf() RETURNS int AS $$return_next 1$$ LANGUAGE pltcl;
select non_srf();

CREATE FUNCTION bad_record_srf(OUT a text, OUT b text) RETURNS SETOF record AS $$
return_next [list a]
$$ LANGUAGE pltcl;
SELECT bad_record_srf();

CREATE FUNCTION bad_field_srf(OUT a text, OUT b text) RETURNS SETOF record AS $$
return_next [list a 1 b 2 cow 3]
$$ LANGUAGE pltcl;
SELECT bad_field_srf();
