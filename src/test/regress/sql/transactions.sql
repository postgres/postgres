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
	BEGIN;
		DROP TABLE foo;
		CREATE TABLE bar (a int);
	ROLLBACK;
	BEGIN;
		CREATE TABLE baz (a int);
	COMMIT;
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
	BEGIN;
		INSERT into bar VALUES (1);
	ROLLBACK;
	BEGIN;
		INSERT into barbaz VALUES (1);
	COMMIT;
	BEGIN;
		BEGIN;
			INSERT INTO foo VALUES (2);
		COMMIT;
	ROLLBACK;
	INSERT INTO foo VALUES (3);
COMMIT;
SELECT * FROM foo;		-- should have 1 and 3
SELECT * FROM barbaz;	-- should have 1

-- check that starting a subxact in a failed xact or subxact works
BEGIN;
	SELECT 0/0;		-- fail the outer xact
	BEGIN;
		SELECT 1;	-- this should NOT work
	COMMIT;
	SELECT 1;		-- this should NOT work
	BEGIN;
		SELECT 1;	-- this should NOT work
	ROLLBACK;
	SELECT 1;		-- this should NOT work
COMMIT;
SELECT 1;			-- this should work

BEGIN;
	BEGIN;
		SELECT 1;	-- this should work
		SELECT 0/0;	-- fail the subxact
		SELECT 1;	-- this should NOT work
		BEGIN;
			SELECT 1;	-- this should NOT work
		ROLLBACK;
		BEGIN;
			SELECT 1;	-- this should NOT work
		COMMIT;
		SELECT 1;	-- this should NOT work
	ROLLBACK;
	SELECT 1;		-- this should work
COMMIT;
SELECT 1;			-- this should work

-- check non-transactional behavior of cursors
BEGIN;
	DECLARE c CURSOR FOR SELECT unique2 FROM tenk1;
	BEGIN;
		FETCH 10 FROM c;
	ROLLBACK;
	BEGIN;
		FETCH 10 FROM c;
	COMMIT;
	FETCH 10 FROM c;
	CLOSE c;
	DECLARE c CURSOR FOR SELECT unique2/0 FROM tenk1;
	BEGIN;
		FETCH 10 FROM c;
	ROLLBACK;
	-- c is now dead to the world ...
	BEGIN;
		FETCH 10 FROM c;
	ROLLBACK;
	FETCH 10 FROM c;
COMMIT;


DROP TABLE foo;
DROP TABLE baz;
DROP TABLE barbaz;
