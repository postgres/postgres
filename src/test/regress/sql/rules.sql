--
-- RULES
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
	where a = old.a;
create rule rtest_v1_del as on delete to rtest_v1 do instead
	delete from rtest_t1 where a = old.a;

--
-- Tables and rules for the constraint update/delete test
--
-- Note:
-- 	Now that we have multiple action rule support, we check
-- 	both possible syntaxes to define them (The last action
--  can but must not have a semicolon at the end).
--
create table rtest_system (sysname text, sysdesc text);
create table rtest_interface (sysname text, ifname text);
create table rtest_person (pname text, pdesc text);
create table rtest_admin (pname text, sysname text);

create rule rtest_sys_upd as on update to rtest_system do (
	update rtest_interface set sysname = new.sysname 
		where sysname = old.sysname;
	update rtest_admin set sysname = new.sysname 
		where sysname = old.sysname
	);

create rule rtest_sys_del as on delete to rtest_system do (
	delete from rtest_interface where sysname = old.sysname;
	delete from rtest_admin where sysname = old.sysname;
	);

create rule rtest_pers_upd as on update to rtest_person do 
	update rtest_admin set pname = new.pname where pname = old.pname;

create rule rtest_pers_del as on delete to rtest_person do 
	delete from rtest_admin where pname = old.pname;

--
-- Tables and rules for the logging test
--
create table rtest_emp (ename char(20), salary money);
create table rtest_emplog (ename char(20), who name, action char(10), newsal money, oldsal money);
create table rtest_empmass (ename char(20), salary money);

create rule rtest_emp_ins as on insert to rtest_emp do
	insert into rtest_emplog values (new.ename, current_user,
			'hired', new.salary, '0.00');

create rule rtest_emp_upd as on update to rtest_emp where new.salary != old.salary do
	insert into rtest_emplog values (new.ename, current_user,
			'honored', new.salary, old.salary);

create rule rtest_emp_del as on delete to rtest_emp do
	insert into rtest_emplog values (old.ename, current_user,
			'fired', '0.00', old.salary);

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
** DO INSTEAD DELETE FROM rtest_t1 WHERE a = old.a
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

-- Note: use ORDER BY here to ensure consistent output across all systems.
-- The above UPDATE affects two rows with equal keys, so they could be
-- updated in either order depending on the whim of the local qsort().

select * from rtest_admin order by pname, sysname;

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

select ename, who = current_user as "matches user", action, newsal, oldsal from rtest_emplog order by ename, action, newsal;
insert into rtest_empmass values ('meyer', '4000.00');
insert into rtest_empmass values ('maier', '5000.00');
insert into rtest_empmass values ('mayr', '6000.00');
insert into rtest_emp select * from rtest_empmass;
select ename, who = current_user as "matches user", action, newsal, oldsal from rtest_emplog order by ename, action, newsal;
update rtest_empmass set salary = salary + '1000.00';
update rtest_emp set salary = rtest_empmass.salary where ename = rtest_empmass.ename;
select ename, who = current_user as "matches user", action, newsal, oldsal from rtest_emplog order by ename, action, newsal;
delete from rtest_emp where ename = rtest_empmass.ename;
select ename, who = current_user as "matches user", action, newsal, oldsal from rtest_emplog order by ename, action, newsal;

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

create table rtest_view1 (a int4, b text, v bool);
create table rtest_view2 (a int4);
create table rtest_view3 (a int4, b text);
create table rtest_view4 (a int4, b text, c int4);
create view rtest_vview1 as select a, b from rtest_view1 X 
	where 0 < (select count(*) from rtest_view2 Y where Y.a = X.a);
create view rtest_vview2 as select a, b from rtest_view1 where v;
create view rtest_vview3 as select a, b from rtest_vview2 X
	where 0 < (select count(*) from rtest_view2 Y where Y.a = X.a);
create view rtest_vview4 as select X.a, X.b, count(Y.a) as refcount
	from rtest_view1 X, rtest_view2 Y
	where X.a = Y.a
	group by X.a, X.b;
create function rtest_viewfunc1(int4) returns int4 as
	'select count(*) from rtest_view2 where a = $1'
	language 'sql';
create view rtest_vview5 as select a, b, rtest_viewfunc1(a) as refcount
	from rtest_view1;

insert into rtest_view1 values (1, 'item 1', 't');
insert into rtest_view1 values (2, 'item 2', 't');
insert into rtest_view1 values (3, 'item 3', 't');
insert into rtest_view1 values (4, 'item 4', 'f');
insert into rtest_view1 values (5, 'item 5', 't');
insert into rtest_view1 values (6, 'item 6', 'f');
insert into rtest_view1 values (7, 'item 7', 't');
insert into rtest_view1 values (8, 'item 8', 't');

insert into rtest_view2 values (2);
insert into rtest_view2 values (2);
insert into rtest_view2 values (4);
insert into rtest_view2 values (5);
insert into rtest_view2 values (7);
insert into rtest_view2 values (7);
insert into rtest_view2 values (7);
insert into rtest_view2 values (7);

select * from rtest_vview1;
select * from rtest_vview2;
select * from rtest_vview3;
select * from rtest_vview4;
select * from rtest_vview5;

insert into rtest_view3 select * from rtest_vview1 where a < 7;
select * from rtest_view3;
delete from rtest_view3;

insert into rtest_view3 select * from rtest_vview2 where a != 5 and b !~ '2';
select * from rtest_view3;
delete from rtest_view3;

insert into rtest_view3 select * from rtest_vview3;
select * from rtest_view3;
delete from rtest_view3;

insert into rtest_view4 select * from rtest_vview4 where 3 > refcount;
select * from rtest_view4;
delete from rtest_view4;

insert into rtest_view4 select * from rtest_vview5 where a > 2 and refcount = 0;
select * from rtest_view4;
delete from rtest_view4;
--
-- Test for computations in views
--
create table rtest_comp (
	part	text,
	unit	char(4),
	size	float
);


create table rtest_unitfact (
	unit	char(4),
	factor	float
);

create view rtest_vcomp as 
	select X.part, (X.size * Y.factor) as size_in_cm
			from rtest_comp X, rtest_unitfact Y
			where X.unit = Y.unit;


insert into rtest_unitfact values ('m', 100.0);
insert into rtest_unitfact values ('cm', 1.0);
insert into rtest_unitfact values ('inch', 2.54);

insert into rtest_comp values ('p1', 'm', 5.0);
insert into rtest_comp values ('p2', 'm', 3.0);
insert into rtest_comp values ('p3', 'cm', 5.0);
insert into rtest_comp values ('p4', 'cm', 15.0);
insert into rtest_comp values ('p5', 'inch', 7.0);
insert into rtest_comp values ('p6', 'inch', 4.4);

select * from rtest_vcomp order by part;

select * from rtest_vcomp where size_in_cm > 10.0 order by size_in_cm using >;

--
-- In addition run the (slightly modified) queries from the
-- programmers manual section on the rule system.
--
CREATE TABLE shoe_data (
	shoename   char(10),      -- primary key
	sh_avail   integer,       -- available # of pairs
	slcolor    char(10),      -- preferred shoelace color
	slminlen   float,         -- miminum shoelace length
	slmaxlen   float,         -- maximum shoelace length
	slunit     char(8)        -- length unit
);

CREATE TABLE shoelace_data (
	sl_name    char(10),      -- primary key
	sl_avail   integer,       -- available # of pairs
	sl_color   char(10),      -- shoelace color
	sl_len     float,         -- shoelace length
	sl_unit    char(8)        -- length unit
);

CREATE TABLE unit (
	un_name    char(8),       -- the primary key
	un_fact    float          -- factor to transform to cm
);

CREATE VIEW shoe AS
	SELECT sh.shoename,
		   sh.sh_avail,
		   sh.slcolor,
		   sh.slminlen,
		   sh.slminlen * un.un_fact AS slminlen_cm,
		   sh.slmaxlen,
		   sh.slmaxlen * un.un_fact AS slmaxlen_cm,
		   sh.slunit
	  FROM shoe_data sh, unit un
	 WHERE sh.slunit = un.un_name;

CREATE VIEW shoelace AS
	SELECT s.sl_name,
		   s.sl_avail,
		   s.sl_color,
		   s.sl_len,
		   s.sl_unit,
		   s.sl_len * u.un_fact AS sl_len_cm
	  FROM shoelace_data s, unit u
	 WHERE s.sl_unit = u.un_name;

CREATE VIEW shoe_ready AS
	SELECT rsh.shoename,
		   rsh.sh_avail,
		   rsl.sl_name,
		   rsl.sl_avail,
		   int4smaller(rsh.sh_avail, rsl.sl_avail) AS total_avail
	  FROM shoe rsh, shoelace rsl
	 WHERE rsl.sl_color = rsh.slcolor
	   AND rsl.sl_len_cm >= rsh.slminlen_cm
	   AND rsl.sl_len_cm <= rsh.slmaxlen_cm;

INSERT INTO unit VALUES ('cm', 1.0);
INSERT INTO unit VALUES ('m', 100.0);
INSERT INTO unit VALUES ('inch', 2.54);

INSERT INTO shoe_data VALUES ('sh1', 2, 'black', 70.0, 90.0, 'cm');
INSERT INTO shoe_data VALUES ('sh2', 0, 'black', 30.0, 40.0, 'inch');
INSERT INTO shoe_data VALUES ('sh3', 4, 'brown', 50.0, 65.0, 'cm');
INSERT INTO shoe_data VALUES ('sh4', 3, 'brown', 40.0, 50.0, 'inch');

INSERT INTO shoelace_data VALUES ('sl1', 5, 'black', 80.0, 'cm');
INSERT INTO shoelace_data VALUES ('sl2', 6, 'black', 100.0, 'cm');
INSERT INTO shoelace_data VALUES ('sl3', 0, 'black', 35.0 , 'inch');
INSERT INTO shoelace_data VALUES ('sl4', 8, 'black', 40.0 , 'inch');
INSERT INTO shoelace_data VALUES ('sl5', 4, 'brown', 1.0 , 'm');
INSERT INTO shoelace_data VALUES ('sl6', 0, 'brown', 0.9 , 'm');
INSERT INTO shoelace_data VALUES ('sl7', 7, 'brown', 60 , 'cm');
INSERT INTO shoelace_data VALUES ('sl8', 1, 'brown', 40 , 'inch');

-- SELECTs in doc
SELECT * FROM shoelace ORDER BY sl_name;
SELECT * FROM shoe_ready WHERE total_avail >= 2;

    CREATE TABLE shoelace_log (
        sl_name    char(10),      -- shoelace changed
        sl_avail   integer,       -- new available value
        log_who    name,          -- who did it
        log_when   datetime       -- when
    );

-- Want "log_who" to be CURRENT_USER,
-- but that is non-portable for the regression test
-- - thomas 1999-02-21

    CREATE RULE log_shoelace AS ON UPDATE TO shoelace_data
        WHERE NEW.sl_avail != OLD.sl_avail
        DO INSERT INTO shoelace_log VALUES (
                                        NEW.sl_name,
                                        NEW.sl_avail,
                                        'Al Bundy',
                                        'epoch'::text
                                    );

UPDATE shoelace_data SET sl_avail = 6 WHERE  sl_name = 'sl7';

SELECT * FROM shoelace_log;

    CREATE RULE shoelace_ins AS ON INSERT TO shoelace
        DO INSTEAD
        INSERT INTO shoelace_data VALUES (
               NEW.sl_name,
               NEW.sl_avail,
               NEW.sl_color,
               NEW.sl_len,
               NEW.sl_unit);

    CREATE RULE shoelace_upd AS ON UPDATE TO shoelace
        DO INSTEAD
        UPDATE shoelace_data SET
               sl_name = NEW.sl_name,
               sl_avail = NEW.sl_avail,
               sl_color = NEW.sl_color,
               sl_len = NEW.sl_len,
               sl_unit = NEW.sl_unit
         WHERE sl_name = OLD.sl_name;

    CREATE RULE shoelace_del AS ON DELETE TO shoelace
        DO INSTEAD
        DELETE FROM shoelace_data
         WHERE sl_name = OLD.sl_name;

    CREATE TABLE shoelace_arrive (
        arr_name    char(10),
        arr_quant   integer
    );

    CREATE TABLE shoelace_ok (
        ok_name     char(10),
        ok_quant    integer
    );

    CREATE RULE shoelace_ok_ins AS ON INSERT TO shoelace_ok
        DO INSTEAD
        UPDATE shoelace SET
               sl_avail = sl_avail + NEW.ok_quant
         WHERE sl_name = NEW.ok_name;

INSERT INTO shoelace_arrive VALUES ('sl3', 10);
INSERT INTO shoelace_arrive VALUES ('sl6', 20);
INSERT INTO shoelace_arrive VALUES ('sl8', 20);

SELECT * FROM shoelace ORDER BY sl_name;

insert into shoelace_ok select * from shoelace_arrive;

SELECT * FROM shoelace ORDER BY sl_name;

SELECT * FROM shoelace_log ORDER BY sl_name;

    CREATE VIEW shoelace_obsolete AS
	SELECT * FROM shoelace WHERE NOT EXISTS
	    (SELECT shoename FROM shoe WHERE slcolor = sl_color);

    CREATE VIEW shoelace_candelete AS
	SELECT * FROM shoelace_obsolete WHERE sl_avail = 0;

insert into shoelace values ('sl9', 0, 'pink', 35.0, 'inch', 0.0);
insert into shoelace values ('sl10', 1000, 'magenta', 40.0, 'inch', 0.0);

SELECT * FROM shoelace_obsolete;
SELECT * FROM shoelace_candelete;

DELETE FROM shoelace WHERE EXISTS
    (SELECT * FROM shoelace_candelete
             WHERE sl_name = shoelace.sl_name);

SELECT * FROM shoelace ORDER BY sl_name;


--
-- Check that ruleutils are working
--
SELECT viewname, definition FROM pg_views ORDER BY viewname;

SELECT tablename, rulename, definition FROM pg_rules 
	ORDER BY tablename, rulename;

