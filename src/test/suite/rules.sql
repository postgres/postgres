---------------------------------------------------------------------------
--
-- rules.sql-
--    test rules
--
--
-- Copyright (c) 1994-5, Regents of the University of California
--
-- $Id: rules.sql,v 1.1.1.1 1996/07/09 06:22:30 scrappy Exp $
--
---------------------------------------------------------------------------

-- test rules creation
create table foo (x int4);
-- instead rules are not working right now
-- create rule rule1 as on select to foo.x do instead update foo set x = 2;
-- select rulename, ev_class, ev_type from pg_rewrite;
select * from foo;

create table bar (x int4, y float4);
create rule rule1 as on insert to bar do insert into foo (x) values (new.x);
insert into bar (x,y) values (10, -10.0);
insert into bar (x,y) values (20, -20.0);
insert into bar (x,y) values (30, 3.14159);

select * from bar;
select * from foo;
drop table foo, bar;

