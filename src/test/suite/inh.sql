---------------------------------------------------------------------------
--
-- inh.sql-
--    checks inheritance 
--
--
-- Copyright (c) 1994, Regents of the University of California
--
-- $Id: inh.sql,v 1.1.1.1 1996/07/09 06:22:30 scrappy Exp $
--
---------------------------------------------------------------------------

create table person (name text, age int4, location point);
create table man () inherits(person);
create table emp (salary int4, manager char16) inherits(person);
create table student (gpa float8) inherits (person);
create table stud_emp (percent int4) inherits (emp, student);
create table female_stud_emp () inherits(stud_emp);

-- attr order: name, age, location
select * from person;
select * from man;
-- attr order: name, age, location, salary, manager
select * from emp;
-- attr order: name, age, location, gpa
select * from student;
-- attr order: name, age, location, salary, manager, gpa, percent
select * from stud_emp;
select * from female_stud_emp;

insert into person values ('andy', 14, '(1,1)');
insert into emp values ('betty', 20, '(2, 1)', 1000, 'mandy');
insert into student values ('cy', 45, '(3, 2)', 1.9);
insert into stud_emp values ('danny', 19, '(3.3, 4.55)', 400, 'mandy', 3.9);
insert into man values ('fred', 2, '(0, 0)');
insert into female_stud_emp values ('gina', 16, '(10, 10)', 500, 'mandy', 3.0);

-- andy
select * from person;

-- betty
select * from emp;

-- cy
select * from student;

-- danny
select * from stud_emp;

-- fred
select * from man;

-- gina
select * from female_stud_emp;

-- andy, betty, cy, danny, fred, gina
select * from person*;

-- betty, danny, gina
select * from emp*;

-- cy, danny, gina
select * from student*;

-- danny, gina
select * from stud_emp*;

drop table female_stud_emp;
drop table stud_emp;
drop table student;
drop table emp;
drop table man;
drop table person;
