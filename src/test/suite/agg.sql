---------------------------------------------------------------------------
--
-- agg.sql-
--    test aggregates
--
--
-- Copyright (c) 1994-5, Regents of the University of California
--
-- $Id: agg.sql,v 1.1.1.1 1996/07/09 06:22:30 scrappy Exp $
--
---------------------------------------------------------------------------

create table agga (a integer);
create table aggb (b smallint);
create table aggc (c float);
create table aggd (d float8);
insert into agga values (1);
insert into agga values (1);
insert into agga values (4);
insert into agga values (3);
select * from agga;
insert into aggb values (10);
insert into aggb values (45);
insert into aggb values (10);
insert into aggb values (30);
select * from aggb;
insert into aggc values (210.3);
insert into aggc values (4.45);
insert into aggc values (310);
insert into aggc values (310);
select * from aggc;
insert into aggd values ('-210.3'::float8);
insert into aggd values ('210.3'::float8);
insert into aggd values ('4.45'::float8);
insert into aggd values ('10310.33336'::float8);
insert into aggd values ('10310.33335'::float8);
select * from aggd;

select count(*) from agga;
select count(*), avg(a) from agga;
select avg(a), max(a) from agga;
select sum(a), max(a) from agga;

select avg(c) from aggc;
select sum(c) from aggc;
select max(c) from aggc;
select min(c) from aggc;

select count(*), avg(a), sum(a), max(a), min(a) from agga;
select count(*), avg(b), sum(b), max(b), min(b) from aggb;
select count(*), avg(c), sum(c), max(c), min(c) from aggc;
select count(*), avg(d), sum(d), max(d), min(d) from aggd;

create table agge (e integer);
-- aggregates on an empty table
select count(*) from agge;
select avg(e) from agge;
select sum(e) from agge;
select sum(e) from agge;
select min(e) from agge;

create table aggf (x int, y int);
insert into aggf (x) values (1);
insert into aggf (y) values (2);
insert into aggf values (10, 20);
select * from aggf;
select count(*) from aggf;
select count(x), count(y) from aggf;
select avg(x), avg(y) from aggf;

drop table agga;
drop table aggb;
drop table aggc;
drop table aggd;
drop table agge;
drop table aggf;
