---------------------------------------------------------------------------
--
-- views.sql-
--    test views queries
--
--
-- Copyright (c) 1994-5, Regents of the University of California
--
-- $Id: views.sql,v 1.1.1.1 1996/07/09 06:22:30 scrappy Exp $
--
---------------------------------------------------------------------------

-- create a real table first
create table v1 (x int4, y int4, z int4);
insert into v1 values (1, 2, 3);
insert into v1 values (1, 3, 4);
insert into v1 values (1, 4, 5);
insert into v1 values (1, 2, 6);

-- create views for selecting single column
create view vv1 as select x from v1;
create view vv2 as select y from v1;
create view vv3 as select z from v1;
select * from vv1;
select * from vv2;
select * from vv3;
drop view vv2;
drop view vv3;

-- create views for selecting column(s) from another view
create view vv as select * from vv1;
select * from vv;

create view vv2 as select x from vv;
select * from vv2;
drop view vv;
drop view vv1;
drop view vv2;

-- create views for selecting multiple columns 
create view vv1 as select x, z from v1;
create view vv2 as select y, z from v1;
create view vv3 as select y, z, x from v1;
select * from vv1;
select * from vv2;
select * from vv3;
drop view vv1;
drop view vv2;
drop view vv3;

-- create views with expressions
create view vv1 as select x as a, z as b, y as c from v1;
select * from vv1;
drop view vv1;

create view vv1 as select z, 100 as p, x as q from v1;
select * from vv1;
drop view vv1;

create view vv1 as select x + y as xy, z from v1;
select * from vv1;
drop view vv1;

-- create views of joins
create table v2 (a int4);
insert into v2 values (2);
insert into v2 values (3);

create view vv1 as select y, z from v1, v2 where y = a;
select * from vv1;
drop view vv1;

create view vv1 as select y - x as yx, z, a from v1, v2 where (x + y) > 3;
select * from vv1;
drop view vv1;

drop table v1, v2;
