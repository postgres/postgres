---------------------------------------------------------------------------
--
-- joins.sql-
--    test joins
--
--
-- Copyright (c) 1994, Regents of the University of California
--
-- $Id: join.sql,v 1.1.1.1 1996/07/09 06:22:30 scrappy Exp $
--
---------------------------------------------------------------------------

create table foo (x int4, y int4);
create table bar (p int4, q int4);
create table baz (a int4, b int4);

insert into foo values (1, 1);
insert into foo values (2, 2);
insert into bar values (1, 1);
insert into baz values (1, 1);
insert into baz values (2, 2);

select * from foo,bar,baz 
where foo.x=bar.p and bar.p=baz.a and baz.b=foo.y;

select * from foo,bar,baz 
where foo.y=bar.p and bar.p=baz.a and baz.b=foo.x and foo.y=bar.q;

select * from foo,bar,baz 
where foo.x=bar.q and bar.p=baz.b and baz.b=foo.y and foo.y=bar.q 
  and bar.p=baz.a;

select * from foo,bar,baz 
where foo.y=bar.p and bar.q=baz.b and baz.b=foo.x and foo.x=bar.q 
  and bar.p=baz.a and bar.p=baz.a;

select bar.p from foo, bar;
select foo.x from foo, bar where foo.x = bar.p;

drop table foo, bar, baz;
