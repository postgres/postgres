---------------------------------------------------------------------------
--
-- oper.sql-
--    test operators
--
--
-- Copyright (c) 1994, Regents of the University of California
--
-- $Id: oper.sql,v 1.1.1.1 1996/07/09 06:22:30 scrappy Exp $
--
---------------------------------------------------------------------------

-- test creation
create operator ##+ (leftarg=int4, rightarg=int4, procedure = int4pl);\g
create operator ##+ (rightarg=int4, procedure=int4fac);\g
create operator ##+ (leftarg=int4, procedure=int4inc);\g

select 4 ##+ 4;\g
select ##+ 4;\g

-- why "select 4 ##+" does not work?
select (4 ##+);\g

drop operator ##+(int4,int4);\g
drop operator ##+(none, int4);\g
drop operator ##+(int4, none);\g

