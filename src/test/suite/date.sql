---------------------------------------------------------------------------
--
-- date.sql-
--    test DATE adt
--
--
-- Copyright (c) 1994-5, Regents of the University of California
--
-- $Id: date.sql,v 1.1.1.1 1996/07/09 06:22:30 scrappy Exp $
--
---------------------------------------------------------------------------

create table dd (d date);
insert into dd values ('06-22-1995');
insert into dd values ('05-31-1994');
insert into dd values ('02-29-1996');
insert into dd values ('12-02-1993');
insert into dd values ('05-31-1994');
insert into dd values ('10-20-1970');
select * from dd;
select * from dd order by d;
select * from dd order by d using >;
select * from dd where d = '05-31-1994';
select * from dd where d <> '05-31-1994';
select * from dd where d < '05-31-1994';
select * from dd where d <= '05-31-1994';
select * from dd where d > '05-31-1994';
select * from dd where d >= '05-31-1994';
create index dd_ind on dd using btree (d date_ops);
drop table dd;
