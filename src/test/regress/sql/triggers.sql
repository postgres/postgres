
create table pkeys (pkey1 int4 not null, pkey2 text not null);
create table fkeys (fkey1 int4, fkey2 text, fkey3 int);
create table fkeys2 (fkey21 int4, fkey22 text, pkey23 int not null);

create index fkeys_i on fkeys (fkey1, fkey2);
create index fkeys2_i on fkeys2 (fkey21, fkey22);
create index fkeys2p_i on fkeys2 (pkey23);

insert into pkeys values (10, '1');
insert into pkeys values (20, '2');
insert into pkeys values (30, '3');
insert into pkeys values (40, '4');
insert into pkeys values (50, '5');
insert into pkeys values (60, '6');
create unique index pkeys_i on pkeys (pkey1, pkey2);

--
-- For fkeys:
-- 	(fkey1, fkey2)	--> pkeys (pkey1, pkey2)
-- 	(fkey3)		--> fkeys2 (pkey23)
--
create trigger check_fkeys_pkey_exist 
	before insert or update on fkeys 
	for each row 
	execute procedure 
	check_primary_key ('fkey1', 'fkey2', 'pkeys', 'pkey1', 'pkey2');

create trigger check_fkeys_pkey2_exist 
	before insert or update on fkeys 
	for each row 
	execute procedure check_primary_key ('fkey3', 'fkeys2', 'pkey23');

--
-- For fkeys2:
-- 	(fkey21, fkey22)	--> pkeys (pkey1, pkey2)
--
create trigger check_fkeys2_pkey_exist 
	before insert or update on fkeys2 
	for each row 
	execute procedure 
	check_primary_key ('fkey21', 'fkey22', 'pkeys', 'pkey1', 'pkey2');

--
-- For pkeys:
-- 	ON DELETE/UPDATE (pkey1, pkey2) CASCADE:
-- 		fkeys (fkey1, fkey2) and fkeys2 (fkey21, fkey22)
--
create trigger check_pkeys_fkey_cascade
	before delete or update on pkeys 
	for each row 
	execute procedure 
	check_foreign_key (2, 'cascade', 'pkey1', 'pkey2', 
	'fkeys', 'fkey1', 'fkey2', 'fkeys2', 'fkey21', 'fkey22');

--
-- For fkeys2:
-- 	ON DELETE/UPDATE (pkey23) RESTRICT:
-- 		fkeys (fkey3)
--
create trigger check_fkeys2_fkey_restrict 
	before delete or update on fkeys2
	for each row 
	execute procedure check_foreign_key (1, 'restrict', 'pkey23', 'fkeys', 'fkey3');

insert into fkeys2 values (10, '1', 1);
insert into fkeys2 values (30, '3', 2);
insert into fkeys2 values (40, '4', 5);
insert into fkeys2 values (50, '5', 3);
-- no key in pkeys
insert into fkeys2 values (70, '5', 3);

insert into fkeys values (10, '1', 2);
insert into fkeys values (30, '3', 3);
insert into fkeys values (40, '4', 2);
insert into fkeys values (50, '5', 2);
-- no key in pkeys
insert into fkeys values (70, '5', 1);
-- no key in fkeys2
insert into fkeys values (60, '6', 4);

delete from pkeys where pkey1 = 30 and pkey2 = '3';
delete from pkeys where pkey1 = 40 and pkey2 = '4';
update pkeys set pkey1 = 7, pkey2 = '70' where pkey1 = 50 and pkey2 = '5';
update pkeys set pkey1 = 7, pkey2 = '70' where pkey1 = 10 and pkey2 = '1';

DROP TABLE pkeys;
DROP TABLE fkeys;
DROP TABLE fkeys2;

-- -- I've disabled the funny_dup17 test because the new semantics
-- -- of AFTER ROW triggers, which get now fired at the end of a
-- -- query allways, cause funny_dup17 to enter an endless loop.
-- --
-- --      Jan
--
-- create table dup17 (x int4);
-- 
-- create trigger dup17_before 
-- 	before insert on dup17
-- 	for each row 
-- 	execute procedure 
-- 	funny_dup17 ()
-- ;
-- 
-- insert into dup17 values (17);
-- select count(*) from dup17;
-- insert into dup17 values (17);
-- select count(*) from dup17;
-- 
-- drop trigger dup17_before on dup17;
-- 
-- create trigger dup17_after
-- 	after insert on dup17
-- 	for each row 
-- 	execute procedure 
-- 	funny_dup17 ()
-- ;
-- insert into dup17 values (13);
-- select count(*) from dup17 where x = 13;
-- insert into dup17 values (13);
-- select count(*) from dup17 where x = 13;
-- 
-- DROP TABLE dup17;

create sequence ttdummy_seq increment 10 start 0 minvalue 0;

create table tttest (
	price_id	int4, 
	price_val	int4, 
	price_on	int4,
	price_off	int4 default 999999
);

create trigger ttdummy 
	before delete or update on tttest
	for each row 
	execute procedure 
	ttdummy (price_on, price_off);

create trigger ttserial 
	before insert or update on tttest
	for each row 
	execute procedure 
	autoinc (price_on, ttdummy_seq);

insert into tttest values (1, 1, null);
insert into tttest values (2, 2, null);
insert into tttest values (3, 3, 0);

select * from tttest;
delete from tttest where price_id = 2;
select * from tttest;
-- what do we see ?

-- get current prices
select * from tttest where price_off = 999999;

-- change price for price_id == 3
update tttest set price_val = 30 where price_id = 3;
select * from tttest;

-- now we want to change pric_id in ALL tuples
-- this gets us not what we need
update tttest set price_id = 5 where price_id = 3;
select * from tttest;

-- restore data as before last update:
select set_ttdummy(0);
delete from tttest where price_id = 5;
update tttest set price_off = 999999 where price_val = 30;
select * from tttest;

-- and try change price_id now!
update tttest set price_id = 5 where price_id = 3;
select * from tttest;
-- isn't it what we need ?

select set_ttdummy(1);

-- we want to correct some "date"
update tttest set price_on = -1 where price_id = 1;
-- but this doesn't work

-- try in this way
select set_ttdummy(0);
update tttest set price_on = -1 where price_id = 1;
select * from tttest;
-- isn't it what we need ?

-- get price for price_id == 5 as it was @ "date" 35
select * from tttest where price_on <= 35 and price_off > 35 and price_id = 5;

drop table tttest;
drop sequence ttdummy_seq;
