---------------------------------------------------------------------------
--
-- float.sql-
--    test float4, float8 adt
--
--
-- Copyright (c) 1994-5, Regents of the University of California
--
-- $Id: float.sql,v 1.1.1.1 1996/07/09 06:22:30 scrappy Exp $
--
---------------------------------------------------------------------------

--
-- float4
--
create table fl (x float4);
insert into fl values ( 3.14 );
insert into fl values ( 147.0 );
insert into fl values ( 3.14 );
insert into fl values ( -3.14 );
select * from fl;
-- float literals
select * from fl where x = 3.14;
select * from fl where x <> 3.14;
select * from fl where x < 3.14;
select * from fl where x <= 3.14;
select * from fl where x > 3.14;
select * from fl where x >= 3.14;
-- adt constant without cast (test coercion)
select * from fl where x = '3.14';
select * from fl where x <> '3.14';
select * from fl where x < '3.14';
select * from fl where x <= '3.14';
select * from fl where x > '3.14';
select * from fl where x >= '3.14';
-- adt constant with float4 cast (test float4 opers)
select * from fl where x = '3.14'::float4;
select * from fl where x <> '3.14'::float4;
select * from fl where x < '3.14'::float4;
select * from fl where x <= '3.14'::float4;
select * from fl where x > '3.14'::float4;
select * from fl where x >= '3.14'::float4;
-- adt constant with float8 cast (test float48 opers)
select * from fl where x = '3.14'::float8;
select * from fl where x <> '3.14'::float8;
select * from fl where x < '3.14'::float8;
select * from fl where x <= '3.14'::float8;
select * from fl where x > '3.14'::float8;
select * from fl where x >= '3.14'::float8;

-- try other operators
update fl set x = x + 2.2;
select * from fl;
update fl set x = x - 2.2;
select * from fl;
update fl set x = x * 2.2;
select * from fl;
update fl set x = x / 2.2;
select * from fl;

--
-- float8
--
create table fl8 (y float8);
insert into fl8 values ( '3.14'::float8 );
insert into fl8 values ( '147.0'::float8 );
insert into fl8 values ( '3.140000001'::float8 );
insert into fl8 values ( '-3.14'::float8);
select * from fl8;
-- float literals
select * from fl8 where y = 3.14;
select * from fl8 where y <> 3.14;
select * from fl8 where y < 3.14;
select * from fl8 where y <= 3.14;
select * from fl8 where y > 3.14;
select * from fl8 where y >= 3.14;
-- adt constant without cast (test coercion)
select * from fl8 where y = '3.14';
select * from fl8 where y <> '3.14';
select * from fl8 where y < '3.14';
select * from fl8 where y <= '3.14';
select * from fl8 where y > '3.14';
select * from fl8 where y >= '3.14';
-- adt constant with float4 cast (test float84 opers)
select * from fl8 where y = '3.14'::float4;
select * from fl8 where y <> '3.14'::float4;
select * from fl8 where y < '3.14'::float4;
select * from fl8 where y <= '3.14'::float4;
select * from fl8 where y > '3.14'::float4;
select * from fl8 where y >= '3.14'::float4;
-- adt constant with float8 cast (test float8 opers)
select * from fl8 where y = '3.14'::float8;
select * from fl8 where y <> '3.14'::float8;
select * from fl8 where y < '3.14'::float8;
select * from fl8 where y <= '3.14'::float8;
select * from fl8 where y > '3.14'::float8;
select * from fl8 where y >= '3.14'::float8;

-- try other operators
update fl8 set y = y + '2.2'::float8;
select * from fl8;
update fl8 set y = y - '2.2'::float8;
select * from fl8;
update fl8 set y = y * '2.2'::float8;
select * from fl8;
update fl8 set y = y / '2.2'::float8;
select * from fl8;

-- drop tables

drop table fl;
drop table fl8;

