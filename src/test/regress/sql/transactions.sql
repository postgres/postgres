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
UPDATE temptest SET a = 0 WHERE a = 1 AND writetest.a = temptest.a; -- ok
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
	ROLLBACK TO one;
	RELEASE one;
	SAVEPOINT two;
		CREATE TABLE baz (a int);
	RELEASE two;
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
	RELEASE one;
	SAVEPOINT two;
		INSERT into barbaz VALUES (1);
	RELEASE two;
	SAVEPOINT three;
		SAVEPOINT four;
			INSERT INTO foo VALUES (2);
		RELEASE four;
	ROLLBACK TO three;
	RELEASE three;
	INSERT INTO foo VALUES (3);
COMMIT;
SELECT * FROM foo;		-- should have 1 and 3
SELECT * FROM barbaz;	-- should have 1

-- test whole-tree commit
BEGIN;
	SAVEPOINT one;
		SELECT foo;
	ROLLBACK TO one;
	RELEASE one;
	SAVEPOINT two;
		CREATE TABLE savepoints (a int);
		SAVEPOINT three;
			INSERT INTO savepoints VALUES (1);
			SAVEPOINT four;
				INSERT INTO savepoints VALUES (2);
				SAVEPOINT five;
					INSERT INTO savepoints VALUES (3);
				ROLLBACK TO five;
COMMIT;
COMMIT;		-- should not be in a transaction block
SELECT * FROM savepoints;

-- test whole-tree rollback
BEGIN;
	SAVEPOINT one;
		DELETE FROM savepoints WHERE a=1;
	RELEASE one;
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
	RELEASE one;
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
	ROLLBACK TO one;
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
	ROLLBACK TO one;
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
	ROLLBACK TO one;
		INSERT INTO savepoints VALUES (21);
	ROLLBACK TO one;
		INSERT INTO savepoints VALUES (22);
COMMIT;
SELECT a FROM savepoints WHERE a BETWEEN 18 AND 22;

DROP TABLE savepoints;

-- only in a transaction block:
SAVEPOINT one;
ROLLBACK TO one;
RELEASE one;

-- Only "rollback to" allowed in aborted state
BEGIN;
  SAVEPOINT one;
  SELECT 0/0;
  SAVEPOINT two;    -- ignored till the end of ...
  RELEASE one;      -- ignored till the end of ...
  ROLLBACK TO one;
  SELECT 1;
COMMIT;
SELECT 1;			-- this should work

-- check non-transactional behavior of cursors
BEGIN;
	DECLARE c CURSOR FOR SELECT unique2 FROM tenk1;
	SAVEPOINT one;
		FETCH 10 FROM c;
	ROLLBACK TO one;
		FETCH 10 FROM c;
	RELEASE one;
	FETCH 10 FROM c;
	CLOSE c;
	DECLARE c CURSOR FOR SELECT unique2/0 FROM tenk1;
	SAVEPOINT two;
		FETCH 10 FROM c;
	ROLLBACK TO two;
	-- c is now dead to the world ...
		FETCH 10 FROM c;
	ROLLBACK TO two;
	RELEASE two;
	FETCH 10 FROM c;
COMMIT;

DROP TABLE foo;
DROP TABLE baz;
DROP TABLE barbaz;
