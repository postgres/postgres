---------------------------------------------------------------------------
--
-- varchar.sql-
--    test CHAR() and VARCHAR() adts
--
--
-- Copyright (c) 1994-5, Regents of the University of California
--
-- $Id: varchar.sql,v 1.1.1.1 1996/07/09 06:22:30 scrappy Exp $
--
---------------------------------------------------------------------------

-- test char(): insert w/ boundary cases
create table f (x char(5));
insert into f values ('zoo');
insert into f values ('a');
insert into f values ('jet');
insert into f values ('abc');
insert into f values ('');
insert into f values ('a c');
insert into f values ('abxyzxyz');
select * from f;
select * from f where x = 'jet';
select * from f where x <> 'jet';
select * from f where x < 'jet';
select * from f where x <= 'jet';
select * from f where x > 'jet';
select * from f where x >= 'jet';
select * from f where x = 'ab';
select * from f where x <> 'ab';
select * from f where x < 'ab';
select * from f where x <= 'ab';
select * from f where x > 'ab';
select * from f where x >= 'ab';
select * from f order by x;
-- test varchar(): insert w/ boundary cases
create table ff (x varchar(5));
insert into ff values ('a');
insert into ff values ('zoo');
insert into ff values ('jet');
insert into ff values ('abc');
insert into ff values ('');
insert into ff values ('a c');
insert into ff values ('abxyzxyz');
select * from ff;
select * from ff where x = 'jet';
select * from ff where x <> 'jet';
select * from ff where x < 'jet';
select * from ff where x <= 'jet';
select * from ff where x > 'jet';
select * from ff where x >= 'jet';
select * from ff where x = 'ab';
select * from ff where x <> 'ab';
select * from ff where x < 'ab';
select * from ff where x <= 'ab';
select * from ff where x > 'ab';
select * from ff where x >= 'ab';
select * from ff order by x using >;

create index f_ind on f using btree (x bpchar_ops);
create index ff_ind on ff using btree (x varchar_ops);

drop table f;
drop table ff;
