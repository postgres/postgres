--
-- Hot Standby tests
--
-- hs_standby_allowed.sql
--

-- SELECT

select count(*) as should_be_1 from hs1;

select count(*) as should_be_2 from hs2;

select count(*) as should_be_3 from hs3;

COPY hs1 TO '/tmp/copy_test';
\! cat /tmp/copy_test

-- Access sequence directly
select is_called from hsseq;

-- Transactions

begin;
select count(*)  as should_be_1 from hs1;
end;

begin transaction read only;
select count(*)  as should_be_1 from hs1;
end;

begin transaction isolation level repeatable read;
select count(*) as should_be_1 from hs1;
select count(*) as should_be_1 from hs1;
select count(*) as should_be_1 from hs1;
commit;

begin;
select count(*) as should_be_1 from hs1;
commit;

begin;
select count(*) as should_be_1 from hs1;
abort;

start transaction;
select count(*) as should_be_1 from hs1;
commit;

begin;
select count(*) as should_be_1 from hs1;
rollback;

begin;
select count(*) as should_be_1 from hs1;
savepoint s;
select count(*) as should_be_2 from hs2;
commit;

begin;
select count(*) as should_be_1 from hs1;
savepoint s;
select count(*) as should_be_2 from hs2;
release savepoint s;
select count(*) as should_be_2 from hs2;
savepoint s;
select count(*) as should_be_3 from hs3;
rollback to savepoint s;
select count(*) as should_be_2 from hs2;
commit;

-- SET parameters

-- has no effect on read only transactions, but we can still set it
set synchronous_commit = on;
show synchronous_commit;
reset synchronous_commit;

discard temp;
discard all;

-- CURSOR commands

BEGIN;

DECLARE hsc CURSOR FOR select * from hs3;

FETCH next from hsc;
fetch first from hsc;
fetch last from hsc;
fetch 1 from hsc;

CLOSE hsc;

COMMIT;

-- Prepared plans

PREPARE hsp AS select count(*) from hs1;
PREPARE hsp_noexec (integer) AS insert into hs1 values ($1);

EXECUTE hsp;

DEALLOCATE hsp;

-- LOCK

BEGIN;
LOCK hs1 IN ACCESS SHARE MODE;
LOCK hs1 IN ROW SHARE MODE;
LOCK hs1 IN ROW EXCLUSIVE MODE;
COMMIT;

-- LOAD
-- should work, easier if there is no test for that...


-- ALLOWED COMMANDS

CHECKPOINT;

discard all;
