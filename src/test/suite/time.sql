---------------------------------------------------------------------------
--
-- time.sql-
--    test TIME adt
--
--
-- Copyright (c) 1994-5, Regents of the University of California
--
-- $Id: time.sql,v 1.1.1.1 1996/07/09 06:22:30 scrappy Exp $
--
---------------------------------------------------------------------------

create table tt (t time);
insert into tt values ('6:22:19.95');
insert into tt values ('5:31:19.94');
insert into tt values ('2:29:1.996');
insert into tt values ('23:59:59.93');
insert into tt values ('0:0:0.0');
insert into tt values ('2:29:1.996');
select * from tt;
select * from tt order by t;
select * from tt order by t using >;
select * from tt where t = '2:29:1.996';
select * from tt where t <> '2:29:1.996';
select * from tt where t < '2:29:1.996';
select * from tt where t <= '2:29:1.996';
select * from tt where t > '2:29:1.996';
select * from tt where t >= '2:29:1.996';
create index tt_ind on tt using btree (t time_ops);
drop table tt;
