--
-- rules.sql
-- From Jan's original setup_ruletest.sql and run_ruletest.sql
-- - thomas 1998-09-13
--

--
-- Tables and rules for the view test
--
create table rtest_t1 (a int4, b int4);
create table rtest_t2 (a int4, b int4);
create table rtest_t3 (a int4, b int4);

create view rtest_v1 as select * from rtest_t1;
create rule rtest_v1_ins as on insert to rtest_v1 do instead
	insert into rtest_t1 values (new.a, new.b);
create rule rtest_v1_upd as on update to rtest_v1 do instead
	update rtest_t1 set a = new.a, b = new.b
	where a = current.a;
create rule rtest_v1_del as on delete to rtest_v1 do instead
	delete from rtest_t1 where a = current.a;

--
-- Tables and rules for the constraint update/delete test
--
-- Note:
-- 	psql prevents from putting colons into brackets as
-- 	required for multi action rules. So we define single
-- 	rules for each action required for now
--
create table rtest_system (sysname text, sysdesc text);
create table rtest_interface (sysname text, ifname text);
create table rtest_person (pname text, pdesc text);
create table rtest_admin (pname text, sysname text);

create rule rtest_sys_upd1 as on update to rtest_system do 
	update rtest_interface set sysname = new.sysname 
		where sysname = current.sysname;
create rule rtest_sys_upd2 as on update to rtest_system do 
	update rtest_admin set sysname = new.sysname 
		where sysname = current.sysname;

create rule rtest_sys_del1 as on delete to rtest_system do
	delete from rtest_interface where sysname = current.sysname;
create rule rtest_sys_del2 as on delete to rtest_system do
	delete from rtest_admin where sysname = current.sysname;

create rule rtest_pers_upd as on update to rtest_person do 
	update rtest_admin set pname = new.pname where pname = current.pname;

create rule rtest_pers_del as on delete to rtest_person do 
	delete from rtest_admin where pname = current.pname;

--
-- Tables and rules for the logging test
--
create table rtest_emp (ename char(20), salary money);
create table rtest_emplog (ename char(20), who name, action char(10), newsal money, oldsal money);
create table rtest_empmass (ename char(20), salary money);

create rule rtest_emp_ins as on insert to rtest_emp do
	insert into rtest_emplog values (new.ename, current_user,
			'hired', new.salary, '0.00');

create rule rtest_emp_upd as on update to rtest_emp where new.salary != current.salary do
	insert into rtest_emplog values (new.ename, current_user,
			'honored', new.salary, current.salary);

create rule rtest_emp_del as on delete to rtest_emp do
	insert into rtest_emplog values (current.ename, current_user,
			'fired', '0.00', current.salary);

--
-- Tables and rules for the multiple cascaded qualified instead
-- rule test 
--
create table rtest_t4 (a int4, b text);
create table rtest_t5 (a int4, b text);
create table rtest_t6 (a int4, b text);
create table rtest_t7 (a int4, b text);
create table rtest_t8 (a int4, b text);
create table rtest_t9 (a int4, b text);

create rule rtest_t4_ins1 as on insert to rtest_t4
		where new.a >= 10 and new.a < 20 do instead
	insert into rtest_t5 values (new.a, new.b);

create rule rtest_t4_ins2 as on insert to rtest_t4
		where new.a >= 20 and new.a < 30 do
	insert into rtest_t6 values (new.a, new.b);

create rule rtest_t5_ins as on insert to rtest_t5
		where new.a > 15 do
	insert into rtest_t7 values (new.a, new.b);

create rule rtest_t6_ins as on insert to rtest_t6
		where new.a > 25 do instead
	insert into rtest_t8 values (new.a, new.b);

--
-- Tables and rules for the rule fire order test
--
create table rtest_order1 (a int4);
create table rtest_order2 (a int4, b int4, c text);

create sequence rtest_seq;

create rule rtest_order_r3 as on insert to rtest_order1 do instead
	insert into rtest_order2 values (new.a, nextval('rtest_seq'),
		'rule 3 - this should run 3rd or 4th');

create rule rtest_order_r4 as on insert to rtest_order1
		where a < 100 do instead
	insert into rtest_order2 values (new.a, nextval('rtest_seq'),
		'rule 4 - this should run 2nd');

create rule rtest_order_r2 as on insert to rtest_order1 do
	insert into rtest_order2 values (new.a, nextval('rtest_seq'),
		'rule 2 - this should run 1st');

create rule rtest_order_r1 as on insert to rtest_order1 do instead
	insert into rtest_order2 values (new.a, nextval('rtest_seq'),
		'rule 1 - this should run 3rd or 4th');

--
-- Tables and rules for the instead nothing test
--
create table rtest_nothn1 (a int4, b text);
create table rtest_nothn2 (a int4, b text);
create table rtest_nothn3 (a int4, b text);
create table rtest_nothn4 (a int4, b text);

create rule rtest_nothn_r1 as on insert to rtest_nothn1
	where new.a >= 10 and new.a < 20 do instead (select 1);

create rule rtest_nothn_r2 as on insert to rtest_nothn1
	where new.a >= 30 and new.a < 40 do instead nothing;

create rule rtest_nothn_r3 as on insert to rtest_nothn2
	where new.a >= 100 do instead
	insert into rtest_nothn3 values (new.a, new.b);

create rule rtest_nothn_r4 as on insert to rtest_nothn2
	do instead nothing;

--
-- Tests on a view that is select * of a table
-- and has insert/update/delete instead rules to
-- behave close like the real table.
--

--
-- We need test date later
--
insert into rtest_t2 values (1, 21);
insert into rtest_t2 values (2, 22);
insert into rtest_t2 values (3, 23);

insert into rtest_t3 values (1, 31);
insert into rtest_t3 values (2, 32);
insert into rtest_t3 values (3, 33);
insert into rtest_t3 values (4, 34);
insert into rtest_t3 values (5, 35);

-- insert values
insert into rtest_v1 values (1, 11);
insert into rtest_v1 values (2, 12);
select * from rtest_v1;

-- delete with constant expression
delete from rtest_v1 where a = 1;
select * from rtest_v1;
insert into rtest_v1 values (1, 11);
delete from rtest_v1 where b = 12;
select * from rtest_v1;
insert into rtest_v1 values (2, 12);
insert into rtest_v1 values (2, 13);
select * from rtest_v1;
** Remember the delete rule on rtest_v1: It says
** DO INSTEAD DELETE FROM rtest_t1 WHERE a = current.a
** So this time both rows with a = 2 must get deleted
\p
\r
delete from rtest_v1 where b = 12;
select * from rtest_v1;
delete from rtest_v1;

-- insert select
insert into rtest_v1 select * from rtest_t2;
select * from rtest_v1;
delete from rtest_v1;

-- same with swapped targetlist
insert into rtest_v1 (b, a) select b, a from rtest_t2;
select * from rtest_v1;

-- now with only one target attribute
insert into rtest_v1 (a) select a from rtest_t3;
select * from rtest_v1;
select * from rtest_v1 where b isnull;

-- let attribute a differ (must be done on rtest_t1 - see above)
update rtest_t1 set a = a + 10 where b isnull;
delete from rtest_v1 where b isnull;
select * from rtest_v1;

-- now updates with constant expression
update rtest_v1 set b = 42 where a = 2;
select * from rtest_v1;
update rtest_v1 set b = 99 where b = 42;
select * from rtest_v1;
update rtest_v1 set b = 88 where b < 50;
select * from rtest_v1;
delete from rtest_v1;
insert into rtest_v1 select rtest_t2.a, rtest_t3.b where rtest_t2.a = rtest_t3.a;
select * from rtest_v1;

-- updates in a mergejoin
update rtest_v1 set b = rtest_t2.b where a = rtest_t2.a;
select * from rtest_v1;
insert into rtest_v1 select * from rtest_t3;
select * from rtest_v1;
update rtest_t1 set a = a + 10 where b > 30;
select * from rtest_v1;
update rtest_v1 set a = rtest_t3.a + 20 where b = rtest_t3.b;
select * from rtest_v1;

--
-- Test for constraint updates/deletes
--
insert into rtest_system values ('orion', 'Linux Jan Wieck');
insert into rtest_system values ('notjw', 'WinNT Jan Wieck (notebook)');
insert into rtest_system values ('neptun', 'Fileserver');

insert into rtest_interface values ('orion', 'eth0');
insert into rtest_interface values ('orion', 'eth1');
insert into rtest_interface values ('notjw', 'eth0');
insert into rtest_interface values ('neptun', 'eth0');

insert into rtest_person values ('jw', 'Jan Wieck');
insert into rtest_person values ('bm', 'Bruce Momjian');

insert into rtest_admin values ('jw', 'orion');
insert into rtest_admin values ('jw', 'notjw');
insert into rtest_admin values ('bm', 'neptun');

update rtest_system set sysname = 'pluto' where sysname = 'neptun';

select * from rtest_interface;
select * from rtest_admin;

update rtest_person set pname = 'jwieck' where pdesc = 'Jan Wieck';

select * from rtest_admin;

delete from rtest_system where sysname = 'orion';

select * from rtest_interface;
select * from rtest_admin;

--
-- Rule qualification test
--
insert into rtest_emp values ('wiech', '5000.00');
insert into rtest_emp values ('gates', '80000.00');
update rtest_emp set ename = 'wiecx' where ename = 'wiech';
update rtest_emp set ename = 'wieck', salary = '6000.00' where ename = 'wiecx';
update rtest_emp set salary = '7000.00' where ename = 'wieck';
delete from rtest_emp where ename = 'gates';

select ename, who = current_user as "matches user", action, newsal, oldsal from rtest_emplog;
insert into rtest_empmass values ('meyer', '4000.00');
insert into rtest_empmass values ('maier', '5000.00');
insert into rtest_empmass values ('mayr', '6000.00');
insert into rtest_emp select * from rtest_empmass;
select ename, who = current_user as "matches user", action, newsal, oldsal from rtest_emplog;
update rtest_empmass set salary = salary + '1000.00';
update rtest_emp set salary = rtest_empmass.salary where ename = rtest_empmass.ename;
select ename, who = current_user as "matches user", action, newsal, oldsal from rtest_emplog;
delete from rtest_emp where ename = rtest_empmass.ename;
select ename, who = current_user as "matches user", action, newsal, oldsal from rtest_emplog;

--
-- Multiple cascaded qualified instead rule test
--
insert into rtest_t4 values (1, 'Record should go to rtest_t4');
insert into rtest_t4 values (2, 'Record should go to rtest_t4');
insert into rtest_t4 values (10, 'Record should go to rtest_t5');
insert into rtest_t4 values (15, 'Record should go to rtest_t5');
insert into rtest_t4 values (19, 'Record should go to rtest_t5 and t7');
insert into rtest_t4 values (20, 'Record should go to rtest_t4 and t6');
insert into rtest_t4 values (26, 'Record should go to rtest_t4 and t8');
insert into rtest_t4 values (28, 'Record should go to rtest_t4 and t8');
insert into rtest_t4 values (30, 'Record should go to rtest_t4');
insert into rtest_t4 values (40, 'Record should go to rtest_t4');

select * from rtest_t4;
select * from rtest_t5;
select * from rtest_t6;
select * from rtest_t7;
select * from rtest_t8;

delete from rtest_t4;
delete from rtest_t5;
delete from rtest_t6;
delete from rtest_t7;
delete from rtest_t8;

insert into rtest_t9 values (1, 'Record should go to rtest_t4');
insert into rtest_t9 values (2, 'Record should go to rtest_t4');
insert into rtest_t9 values (10, 'Record should go to rtest_t5');
insert into rtest_t9 values (15, 'Record should go to rtest_t5');
insert into rtest_t9 values (19, 'Record should go to rtest_t5 and t7');
insert into rtest_t9 values (20, 'Record should go to rtest_t4 and t6');
insert into rtest_t9 values (26, 'Record should go to rtest_t4 and t8');
insert into rtest_t9 values (28, 'Record should go to rtest_t4 and t8');
insert into rtest_t9 values (30, 'Record should go to rtest_t4');
insert into rtest_t9 values (40, 'Record should go to rtest_t4');

insert into rtest_t4 select * from rtest_t9 where a < 20;

select * from rtest_t4;
select * from rtest_t5;
select * from rtest_t6;
select * from rtest_t7;
select * from rtest_t8;

insert into rtest_t4 select * from rtest_t9 where b ~ 'and t8';

select * from rtest_t4;
select * from rtest_t5;
select * from rtest_t6;
select * from rtest_t7;
select * from rtest_t8;

insert into rtest_t4 select a + 1, b from rtest_t9 where a in (20, 30, 40);

select * from rtest_t4;
select * from rtest_t5;
select * from rtest_t6;
select * from rtest_t7;
select * from rtest_t8;

--
-- Check that the ordering of rules fired is correct
--
insert into rtest_order1 values (1);
select * from rtest_order2;

--
-- Check if instead nothing w/without qualification works
--
insert into rtest_nothn1 values (1, 'want this');
insert into rtest_nothn1 values (2, 'want this');
insert into rtest_nothn1 values (10, 'don''t want this');
insert into rtest_nothn1 values (19, 'don''t want this');
insert into rtest_nothn1 values (20, 'want this');
insert into rtest_nothn1 values (29, 'want this');
insert into rtest_nothn1 values (30, 'don''t want this');
insert into rtest_nothn1 values (39, 'don''t want this');
insert into rtest_nothn1 values (40, 'want this');
insert into rtest_nothn1 values (50, 'want this');
insert into rtest_nothn1 values (60, 'want this');

select * from rtest_nothn1;

insert into rtest_nothn2 values (10, 'too small');
insert into rtest_nothn2 values (50, 'too small');
insert into rtest_nothn2 values (100, 'OK');
insert into rtest_nothn2 values (200, 'OK');

select * from rtest_nothn2;
select * from rtest_nothn3;

delete from rtest_nothn1;
delete from rtest_nothn2;
delete from rtest_nothn3;

insert into rtest_nothn4 values (1, 'want this');
insert into rtest_nothn4 values (2, 'want this');
insert into rtest_nothn4 values (10, 'don''t want this');
insert into rtest_nothn4 values (19, 'don''t want this');
insert into rtest_nothn4 values (20, 'want this');
insert into rtest_nothn4 values (29, 'want this');
insert into rtest_nothn4 values (30, 'don''t want this');
insert into rtest_nothn4 values (39, 'don''t want this');
insert into rtest_nothn4 values (40, 'want this');
insert into rtest_nothn4 values (50, 'want this');
insert into rtest_nothn4 values (60, 'want this');

insert into rtest_nothn1 select * from rtest_nothn4;

select * from rtest_nothn1;

delete from rtest_nothn4;

insert into rtest_nothn4 values (10, 'too small');
insert into rtest_nothn4 values (50, 'too small');
insert into rtest_nothn4 values (100, 'OK');
insert into rtest_nothn4 values (200, 'OK');

insert into rtest_nothn2 select * from rtest_nothn4;

select * from rtest_nothn2;
select * from rtest_nothn3;

