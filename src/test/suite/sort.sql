---------------------------------------------------------------------------
--
-- sort.sql-
--    test sorting
--
--
-- Copyright (c) 1994-5, Regents of the University of California
--
-- $Id: sort.sql,v 1.1.1.1 1996/07/09 06:22:30 scrappy Exp $
--
---------------------------------------------------------------------------

create table s1 (x int4, y int4);
create table s2 (a int4, b int4, c int4);
insert into s1 values (1, 3);
insert into s1 values (2, 3);
insert into s1 values (2, 1);
insert into s2 values (1, 3, 9);
insert into s2 values (1, 4, 9);
insert into s2 values (3, 4, 7);
insert into s2 values (3, 5, 8);
select distinct y from s1;
select a, c from s2;
select distinct a, c from s2;
select distinct a, c from s2 order by c;
select b, c from s2 order by c, b;
select x, b, c from s1, s2 order by b;
select distinct a, x, c from s1, s2 order by c, x;
select x AS p, b AS q, c AS r from s1, s2 order by p;
select x AS p, b AS q, c AS r from s1, s2 order by q;
select x AS p, b AS q, c AS r from s1, s2 order by r;
select x AS p, b AS q, c AS r from s1, s2 order by p, r;
select x AS p, b AS q, c AS r from s1, s2 order by q, r;
select x AS p, b AS q, c AS r from s1, s2 order by q, p;
create table s3 (x int4);
insert into s3 values (3);
insert into s3 values (4);
select * from s1, s3 order by x;
select * from s3, s1 order by x;
create table s4 (a int4, b int4, c int4, d int4, e int4, f int4, g int4, h int4, i int4);
insert into s4 values (1, 1, 1, 1, 1, 1, 1, 1, 2);
insert into s4 values (1, 1, 1, 1, 1, 1, 1, 1, 1);
insert into s4 values (1, 1, 1, 1, 1, 1, 1, 1, 3);
select * from s4 order by a, b, c, d, e, f, g, h;
create table s5 (a int4, b int4);
insert into s5 values (1, 2);
insert into s5 values (1, 3);
insert into s5 values (1, 1);
insert into s5 values (2, 1);
insert into s5 values (2, 4);
insert into s5 values (2, 2);
select * from s5 order by a using <;
select * from s5 order by a using >;
select * from s5 order by a using >, b using <;
select * from s5 order by a using >, b using >;

drop table s1, s2, s3, s4, s5;
