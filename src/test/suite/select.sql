---------------------------------------------------------------------------
--
-- select.sql-
--    test select
--
--
-- Copyright (c) 1994-5, Regents of the University of California
--
-- $Id: select.sql,v 1.1.1.1 1996/07/09 06:22:30 scrappy Exp $
--
---------------------------------------------------------------------------

-- test Result nodes (constant target list/quals)
select 1 as X;
create table foo (name char16, salary int4);
insert into foo values ('mike', 15000);
select * from foo where 2 > 1;
select * from pg_class where 1=0;

-- test select distinct
create table bar (x int4);
insert into bar values (1);
insert into bar values (2);
insert into bar values (1);
select distinct * from bar;
select distinct * into table bar2 from bar;
select distinct * from bar2;

drop table foo;
drop table bar;
drop table bar2;
