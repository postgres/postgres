---------------------------------------------------------------------------
--
-- group.sql-
--    test GROUP BY (with aggregates)
--
--
-- Copyright (c) 1994-5, Regents of the University of California
--
-- $Id: group.sql,v 1.1.1.1 1996/07/09 06:22:30 scrappy Exp $
--
---------------------------------------------------------------------------

create table G (x int4, y int4, z int4);
insert into G values (1, 2, 6);
insert into G values (1, 3, 7);
insert into G values (1, 3, 8);
insert into G values (1, 4, 9);
insert into G values (1, 4, 10);
insert into G values (1, 4, 11);
insert into G values (1, 5, 12);
insert into G values (1, 5, 13);

select x from G group by x;
select y from G group by y;
select z from G group by z;
select x, y from G group by x, y;
select x, y from G group by y, x;
select x, y, z from G group by x, y, z;

-- mixed target list (aggregates and group columns)
select count(y) from G group by y;
select x, count(x) from G group by x;
select y, count(y), sum(G.z) from G group by y;
select sum(G.x), sum(G.y), z from G group by z;
select y, avg(z) from G group by y;

-- group attr not in target list
select sum(x) from G group by y;
select sum(x), sum(z) from G group by y;
select sum(z) from G group by y;

-- aggregates in expressions
select sum(G.z)/count(G.z), avg(G.z) from G group by y;

-- with qualifications
select y, count(y) from G where z < 11 group by y;
select y, count(y) from G where z > 9 group by y;
select y, count(y) from G where z > 8 and z < 12 group by y;
select y, count(y) from G where y = 4 group by y;
select y, count(y) from G where y > 10 group by y;

-- with order by
select y, count(y) as c from G group by y order by c;
select y, count(y) as c from G group by y order by c, y;
select y, count(y) as c from G where z > 20 group by y order by c;
-- just to make sure we didn't screw up order by
select x, y from G order by y, x;

-- with having
-- HAVING clause is not implemented yet
--select count(y) from G having count(y) > 1
--select count(y) from G group by y having y > 3
--select y from G group by y having y > 3
--select y from G where z > 10 group by y having y > 3
--select y from G group by y having y > 10
--select count(G.y) from G group by y having y > 10
--select y from G where z > 20 group by y having y > 3

create table H (a int4, b int4);
insert into H values (3, 9)
insert into H values (4, 13);
create table F (p int4);
insert into F values (7)
insert into F values (11);

-- joins
select y from G, H where G.y = H.a group by y;
select sum(b) from G, H where G.y = H.a group by y;
select y, count(y), sum(b) from G, H where G.y = H.a group by y;
select a, sum(x), sum(b) from G, H where G.y = H.a group by a;
select y, count(*) from G, H where G.z = H.b group by y;
select z, sum(y) from G, H, F where G.y = H.a and G.z = F.p group by z;
select a, avg(p) from G, H, F where G.y = H.a and G.z = F.p group by a;

-- just aggregates
select sum(x) from G, H where G.y = H.a;
select sum(y) from G, H where G.y = H.a;
select sum(a) from G, H where G.y = H.a;
select sum(b) from G, H where G.y = H.a;
select count(*) from G group by y;

insert into G (y, z) values (6, 14);
insert into G (x, z) values (2, 14);
select count(*) from G;
select count(x), count(y), count(z) from G;
select x from G group by x;
select y, count(*) from G group by y;

-- 
drop table G, H, F;
