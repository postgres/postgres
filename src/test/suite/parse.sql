---------------------------------------------------------------------------
--
-- parse.sql-
--    checks the parser
--
--
-- Copyright (c) 1994, Regents of the University of California
--
-- $Id: parse.sql,v 1.1.1.1 1996/07/09 06:22:30 scrappy Exp $
--
---------------------------------------------------------------------------

create table foo (x int4, y int4, z int4);
create table bar (x int4, y int4, z int4);
create table baz (a int4, b int4);

insert into foo values (1, 2, 3);
insert into foo values (4, 5, 6);
insert into foo values (7, 8, 9);
insert into bar values (11, 12, 13);
insert into bar values (14, 15, 16);
insert into bar values (17, 18, 19);
insert into baz values (99, 88);
insert into baz values (77, 66);

-- once upon a time, this becomes a join of foo and f:
select * from foo f where f.x = 4;
select * from foo f, foo where f.x > foo.x;
select * from foo f, foo where f.x = 1 and foo.z > f.z;

-- not standard SQL, POSTQUEL semantics
-- update foo set x = f.x from foo f where foo.x = 1 and f.x = 7
-- select * from foo

-- fix error message:
--select foo.x from foo,bar,baz where foo.x=bar.x and bar.y=baz.x and baz.x=foo.x

-- see if renaming the column works
select y as a, z as b from foo order by a;
select foo.y as a, foo.z as b from foo order by b;

-- column expansion
select foo.*, bar.z, baz.* from foo, bar, baz;

drop table foo, bar, baz;
