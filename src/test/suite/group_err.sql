---------------------------------------------------------------------------
--
-- group_err.sql-
--    test illegal use of GROUP BY (with aggregates)
--
--
-- Copyright (c) 1994-5, Regents of the University of California
--
-- $Id: group_err.sql,v 1.1.1.1 1996/07/09 06:22:30 scrappy Exp $
--
---------------------------------------------------------------------------

create table G_ERR (x int4, y int4, z int4);

select x from G_ERR group by y;
select x, sum(z) from G_ERR group by y;
select x, count(x) from G_ERR;

select max(count(x)) from G_ERR;

select x from G_ERR where count(x) = 1;

create table H_ERR (a int4, b int4);

select y, a, count(y), sum(b) 
from G_ERR, H_ERR 
where G_ERR.y = H_ERR.a group by y;

drop table G_ERR, H_ERR;
