--
-- TRANSACTIONS
--

BEGIN;

SELECT * 
   INTO TABLE xacttest
   FROM aggtest;

INSERT INTO xacttest (a, b) VALUES (777, 777.777);

END;

-- should retrieve one value--
SELECT a FROM xacttest WHERE a > 100;


BEGIN;

CREATE TABLE disappear (a int4);

DELETE FROM aggtest;

-- should be empty
SELECT * FROM aggtest;

ABORT;

-- should not exist 
SELECT oid FROM pg_class WHERE relname = 'disappear';

-- should have members again 
SELECT * FROM aggtest;


-- Read-only tests

CREATE TABLE writetest (a int);
CREATE TEMPORARY TABLE temptest (a int);

SET SESSION CHARACTERISTICS AS TRANSACTION READ ONLY;

DROP TABLE writetest; -- fail
INSERT INTO writetest VALUES (1); -- fail
SELECT * FROM writetest; -- ok
DELETE FROM temptest; -- ok
UPDATE temptest SET a = 0 FROM writetest WHERE temptest.a = 1 AND writetest.a = temptest.a; -- ok
PREPARE test AS UPDATE writetest SET a = 0; -- ok
EXECUTE test; -- fail
SELECT * FROM writetest, temptest; -- ok
CREATE TABLE test AS SELECT * FROM writetest; -- fail

START TRANSACTION READ WRITE;
DROP TABLE writetest; -- ok
COMMIT;

-- Subtransactions, basic tests
-- create & drop tables
SET SESSION CHARACTERISTICS AS TRANSACTION READ WRITE;
CREATE TABLE foobar (a int);
BEGIN;
	CREATE TABLE foo (a int);
	SAVEPOINT one;
		DROP TABLE foo;
		CREATE TABLE bar (a int);
	ROLLBACK TO SAVEPOINT one;
	RELEASE SAVEPOINT one;
	SAVEPOINT two;
		CREATE TABLE baz (a int);
	RELEASE SAVEPOINT two;
	drop TABLE foobar;
	CREATE TABLE barbaz (a int);
COMMIT;
-- should exist: barbaz, baz, foo
SELECT * FROM foo;		-- should be empty
SELECT * FROM bar;		-- shouldn't exist
SELECT * FROM barbaz;	-- should be empty
SELECT * FROM baz;		-- should be empty

-- inserts
BEGIN;
	INSERT INTO foo VALUES (1);
	SAVEPOINT one;
		INSERT into bar VALUES (1);
	ROLLBACK TO one;
	RELEASE SAVEPOINT one;
	SAVEPOINT two;
		INSERT into barbaz VALUES (1);
	RELEASE two;
	SAVEPOINT three;
		SAVEPOINT four;
			INSERT INTO foo VALUES (2);
		RELEASE SAVEPOINT four;
	ROLLBACK TO SAVEPOINT three;
	RELEASE SAVEPOINT three;
	INSERT INTO foo VALUES (3);
COMMIT;
SELECT * FROM foo;		-- should have 1 and 3
SELECT * FROM barbaz;	-- should have 1

-- test whole-tree commit
BEGIN;
	SAVEPOINT one;
		SELECT foo;
	ROLLBACK TO SAVEPOINT one;
	RELEASE SAVEPOINT one;
	SAVEPOINT two;
		CREATE TABLE savepoints (a int);
		SAVEPOINT three;
			INSERT INTO savepoints VALUES (1);
			SAVEPOINT four;
				INSERT INTO savepoints VALUES (2);
				SAVEPOINT five;
					INSERT INTO savepoints VALUES (3);
				ROLLBACK TO SAVEPOINT five;
COMMIT;
COMMIT;		-- should not be in a transaction block
SELECT * FROM savepoints;

-- test whole-tree rollback
BEGIN;
	SAVEPOINT one;
		DELETE FROM savepoints WHERE a=1;
	RELEASE SAVEPOINT one;
	SAVEPOINT two;
		DELETE FROM savepoints WHERE a=1;
		SAVEPOINT three;
			DELETE FROM savepoints WHERE a=2;
ROLLBACK;
COMMIT;		-- should not be in a transaction block
		
SELECT * FROM savepoints;

-- test whole-tree commit on an aborted subtransaction
BEGIN;
	INSERT INTO savepoints VALUES (4);
	SAVEPOINT one;
		INSERT INTO savepoints VALUES (5);
		SELECT foo;
COMMIT;
SELECT * FROM savepoints;

BEGIN;
	INSERT INTO savepoints VALUES (6);
	SAVEPOINT one;
		INSERT INTO savepoints VALUES (7);
	RELEASE SAVEPOINT one;
	INSERT INTO savepoints VALUES (8);
COMMIT;
-- rows 6 and 8 should have been created by the same xact
SELECT a.xmin = b.xmin FROM savepoints a, savepoints b WHERE a.a=6 AND b.a=8;
-- rows 6 and 7 should have been created by different xacts
SELECT a.xmin = b.xmin FROM savepoints a, savepoints b WHERE a.a=6 AND b.a=7;

BEGIN;
	INSERT INTO savepoints VALUES (9);
	SAVEPOINT one;
		INSERT INTO savepoints VALUES (10);
	ROLLBACK TO SAVEPOINT one;
		INSERT INTO savepoints VALUES (11);
COMMIT;
SELECT a FROM savepoints WHERE a in (9, 10, 11);
-- rows 9 and 11 should have been created by different xacts
SELECT a.xmin = b.xmin FROM savepoints a, savepoints b WHERE a.a=9 AND b.a=11;

BEGIN;
	INSERT INTO savepoints VALUES (12);
	SAVEPOINT one;
		INSERT INTO savepoints VALUES (13);
		SAVEPOINT two;
			INSERT INTO savepoints VALUES (14);
	ROLLBACK TO SAVEPOINT one;
		INSERT INTO savepoints VALUES (15);
		SAVEPOINT two;
			INSERT INTO savepoints VALUES (16);
			SAVEPOINT three;
				INSERT INTO savepoints VALUES (17);
COMMIT;
SELECT a FROM savepoints WHERE a BETWEEN 12 AND 17;

BEGIN;
	INSERT INTO savepoints VALUES (18);
	SAVEPOINT one;
		INSERT INTO savepoints VALUES (19);
		SAVEPOINT two;
			INSERT INTO savepoints VALUES (20);
	ROLLBACK TO SAVEPOINT one;
		INSERT INTO savepoints VALUES (21);
	ROLLBACK TO SAVEPOINT one;
		INSERT INTO savepoints VALUES (22);
COMMIT;
SELECT a FROM savepoints WHERE a BETWEEN 18 AND 22;

DROP TABLE savepoints;

-- only in a transaction block:
SAVEPOINT one;
ROLLBACK TO SAVEPOINT one;
RELEASE SAVEPOINT one;

-- Only "rollback to" allowed in aborted state
BEGIN;
  SAVEPOINT one;
  SELECT 0/0;
  SAVEPOINT two;    -- ignored till the end of ...
  RELEASE SAVEPOINT one;      -- ignored till the end of ...
  ROLLBACK TO SAVEPOINT one;
  SELECT 1;
COMMIT;
SELECT 1;			-- this should work

-- check non-transactional behavior of cursors
BEGIN;
	DECLARE c CURSOR FOR SELECT unique2 FROM tenk1 ORDER BY unique2;
	SAVEPOINT one;
		FETCH 10 FROM c;
	ROLLBACK TO SAVEPOINT one;
		FETCH 10 FROM c;
	RELEASE SAVEPOINT one;
	FETCH 10 FROM c;
	CLOSE c;
	DECLARE c CURSOR FOR SELECT unique2/0 FROM tenk1 ORDER BY unique2;
	SAVEPOINT two;
		FETCH 10 FROM c;
	ROLLBACK TO SAVEPOINT two;
	-- c is now dead to the world ...
		FETCH 10 FROM c;
	ROLLBACK TO SAVEPOINT two;
	RELEASE SAVEPOINT two;
	FETCH 10 FROM c;
COMMIT;

--
-- Check that "stable" functions are really stable.  They should not be
-- able to see the partial results of the calling query.  (Ideally we would
-- also check that they don't see commits of concurrent transactions, but
-- that's a mite hard to do within the limitations of pg_regress.)
--
select * from xacttest;

create or replace function max_xacttest() returns smallint language sql as
'select max(a) from xacttest' stable;

begin;
update xacttest set a = max_xacttest() + 10 where a > 0;
select * from xacttest;
rollback;

-- But a volatile function can see the partial results of the calling query
create or replace function max_xacttest() returns smallint language sql as
'select max(a) from xacttest' volatile;

begin;
update xacttest set a = max_xacttest() + 10 where a > 0;
select * from xacttest;
rollback;

-- Now the same test with plpgsql (since it depends on SPI which is different)
create or replace function max_xacttest() returns smallint language plpgsql as
'begin return max(a) from xacttest; end' stable;

begin;
update xacttest set a = max_xacttest() + 10 where a > 0;
select * from xacttest;
rollback;

create or replace function max_xacttest() returns smallint language plpgsql as
'begin return max(a) from xacttest; end' volatile;

begin;
update xacttest set a = max_xacttest() + 10 where a > 0;
select * from xacttest;
rollback;


-- test case for problems with dropping an open relation during abort
BEGIN;
	savepoint x;
		CREATE TABLE koju (a INT UNIQUE);
		INSERT INTO koju VALUES (1);
		INSERT INTO koju VALUES (1);
	rollback to x;

	CREATE TABLE koju (a INT UNIQUE);
	INSERT INTO koju VALUES (1);
	INSERT INTO koju VALUES (1);
ROLLBACK;

DROP TABLE foo;
DROP TABLE baz;
DROP TABLE barbaz;

-- verify that cursors created during an aborted subtransaction are
-- closed, but that we do not rollback the effect of any FETCHs
-- performed in the aborted subtransaction
begin;

savepoint x;
create table abc (a int);
insert into abc values (5);
insert into abc values (10);
declare foo cursor for select * from abc;
fetch from foo;
rollback to x;

-- should fail
fetch from foo;
commit;

begin;

create table abc (a int);
insert into abc values (5);
insert into abc values (10);
insert into abc values (15);
declare foo cursor for select * from abc;

fetch from foo;

savepoint x;
fetch from foo;
rollback to x;

fetch from foo;

abort;

-- tests for the "tid" type
SELECT '(3, 3)'::tid = '(3, 4)'::tid;
SELECT '(3, 3)'::tid = '(3, 3)'::tid;
SELECT '(3, 3)'::tid <> '(3, 3)'::tid;
SELECT '(3, 3)'::tid <> '(3, 4)'::tid;
