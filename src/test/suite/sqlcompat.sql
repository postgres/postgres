---------------------------------------------------------------------------
--
-- sqlcompat.sql-
--    test aliases for SQL basic types and aggregates
--
--
-- Copyright (c) 1994-5, Regents of the University of California
--
-- $Id: sqlcompat.sql,v 1.1.1.1 1996/07/09 06:22:30 scrappy Exp $
--
---------------------------------------------------------------------------

-- check aliases for data types
create table st1 (x int, y integer, z int4);
insert into st1 values (1);
insert into st1 values (10);
select * from st1;
create table st2 (x smallint, y int2);
insert into st2 values (1);
insert into st2 values (10);
select * from st2;
create table st3 (x float, y real, z float4);
insert into st3 values (1);
insert into st3 values (10);
select * from st3;

create table st4 (x float8);
insert into st4 values (1);
insert into st4 values (10);
select * from st4;

-- check aliases for aggregate names
select max(x) from st1;
select min(x) from st1;
select sum(x) from st1;
select avg(x) from st1;

select max(x) from st2;
select min(x) from st2;
select sum(x) from st2;
select avg(x) from st2;

select max(x) from st3;
select min(x) from st3;
select sum(x) from st3;
select avg(x) from st3;

select max(x) from st4;
select min(x) from st4;
select sum(x) from st4;
select avg(x) from st4;

drop table st1;
drop table st2;
drop table st3;
drop table st4;

