--
-- Hot Standby tests
--
-- hs_standby_disallowed.sql
--

SET transaction_read_only = off;

begin transaction read write;
commit;

-- SELECT

select * from hs1 FOR SHARE;
select * from hs1 FOR UPDATE;

-- DML
BEGIN;
insert into hs1 values (37);
ROLLBACK;
BEGIN;
delete from hs1 where col1 = 1;
ROLLBACK;
BEGIN;
update hs1 set col1 = NULL where col1 > 0;
ROLLBACK;
BEGIN;
truncate hs3;
ROLLBACK;

-- DDL

create temporary table hstemp1 (col1 integer);
BEGIN;
drop table hs2;
ROLLBACK;
BEGIN;
create table hs4 (col1 integer);
ROLLBACK;

-- Sequences

SELECT nextval('hsseq');

-- Two-phase commit transaction stuff

BEGIN;
SELECT count(*) FROM hs1;
PREPARE TRANSACTION 'foobar';
ROLLBACK;
BEGIN;
SELECT count(*) FROM hs1;
COMMIT PREPARED 'foobar';
ROLLBACK;

BEGIN;
SELECT count(*) FROM hs1;
PREPARE TRANSACTION 'foobar';
ROLLBACK PREPARED 'foobar';
ROLLBACK;

BEGIN;
SELECT count(*) FROM hs1;
ROLLBACK PREPARED 'foobar';
ROLLBACK;


-- Locks
BEGIN;
LOCK hs1;
COMMIT;
BEGIN;
LOCK hs1 IN SHARE UPDATE EXCLUSIVE MODE;
COMMIT;
BEGIN;
LOCK hs1 IN SHARE MODE;
COMMIT;
BEGIN;
LOCK hs1 IN SHARE ROW EXCLUSIVE MODE;
COMMIT;
BEGIN;
LOCK hs1 IN EXCLUSIVE MODE;
COMMIT;
BEGIN;
LOCK hs1 IN ACCESS EXCLUSIVE MODE;
COMMIT;

-- Listen
listen a;
notify a;

-- disallowed commands

ANALYZE hs1;

VACUUM hs2;

CLUSTER hs2 using hs1_pkey;

REINDEX TABLE hs2;

REVOKE SELECT ON hs1 FROM PUBLIC;
GRANT SELECT ON hs1 TO PUBLIC;
