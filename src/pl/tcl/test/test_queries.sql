
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

select * from T_pkey1 order by key1 using @<, key2;
select * from T_pkey2 order by key1 using @<, key2;

