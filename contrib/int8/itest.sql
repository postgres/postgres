--
-- Test int8 64-bit integers.
--
drop table qtest;
create table qtest(q1 int8, q2 int8);

insert into qtest values('123','456');
insert into qtest values('123','4567890123456789');
insert into qtest values('4567890123456789','123');
insert into qtest values('4567890123456789','4567890123456789');
insert into qtest values('4567890123456789','-4567890123456789');

select * from qtest;

select q1, -q1 as minus from qtest;

select q1, q2, q1 + q2 as plus from qtest;
select q1, q2, q1 - q2 as minus from qtest;
select q1, q2, q1 * q2 as multiply from qtest
 where q1 < 1000 or (q2 > 0 and q2 < 1000);
--select q1, q2, q1 * q2 as multiply qtest
-- where q1 < '1000'::int8 or (q2 > '0'::int8 and q2 < '1000'::int8);
select q1, q2, q1 / q2 as divide from qtest;

select q1, float8(q1) from qtest;
select q2, float8(q2) from qtest;
select q1, int8(float8(q1)) from qtest;
