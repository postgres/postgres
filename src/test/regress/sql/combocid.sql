--
-- Tests for some likely failure cases with combo cmin/cmax mechanism
--
CREATE TEMP TABLE combocidtest (foobar int);

BEGIN;

-- a few dummy ops to push up the CommandId counter
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;

INSERT INTO combocidtest VALUES (1);
INSERT INTO combocidtest VALUES (2);

SELECT ctid,cmin,* FROM combocidtest;

SAVEPOINT s1;

UPDATE combocidtest SET foobar = foobar + 10;

-- here we should see only updated tuples
SELECT ctid,cmin,* FROM combocidtest;

ROLLBACK TO s1;

-- now we should see old tuples, but with combo CIDs starting at 0
SELECT ctid,cmin,* FROM combocidtest;

COMMIT;

-- combo data is not there anymore, but should still see tuples
SELECT ctid,cmin,* FROM combocidtest;

-- Test combo CIDs with portals
BEGIN;

INSERT INTO combocidtest VALUES (333);

DECLARE c CURSOR FOR SELECT ctid,cmin,* FROM combocidtest;

DELETE FROM combocidtest;

FETCH ALL FROM c;

ROLLBACK;

SELECT ctid,cmin,* FROM combocidtest;

-- check behavior with locked tuples
BEGIN;

-- a few dummy ops to push up the CommandId counter
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;

INSERT INTO combocidtest VALUES (444);

SELECT ctid,cmin,* FROM combocidtest;

SAVEPOINT s1;

-- this doesn't affect cmin
SELECT ctid,cmin,* FROM combocidtest FOR UPDATE;
SELECT ctid,cmin,* FROM combocidtest;

-- but this does
UPDATE combocidtest SET foobar = foobar + 10;

SELECT ctid,cmin,* FROM combocidtest;

ROLLBACK TO s1;

SELECT ctid,cmin,* FROM combocidtest;

COMMIT;

SELECT ctid,cmin,* FROM combocidtest;

-- test for bug reported in
-- CABRT9RC81YUf1=jsmWopcKJEro=VoeG2ou6sPwyOUTx_qteRsg@mail.gmail.com
CREATE TABLE IF NOT EXISTS testcase(
	id int PRIMARY KEY,
	balance numeric
);
INSERT INTO testcase VALUES (1, 0);
BEGIN;
SELECT * FROM testcase WHERE testcase.id = 1 FOR UPDATE;
UPDATE testcase SET balance = balance + 400 WHERE id=1;
SAVEPOINT subxact;
UPDATE testcase SET balance = balance - 100 WHERE id=1;
ROLLBACK TO SAVEPOINT subxact;
-- should return one tuple
SELECT * FROM testcase WHERE id = 1 FOR UPDATE;
ROLLBACK;
DROP TABLE testcase;

-- Test for bug reported in
-- CAOG+RQ6ECiiFH7bb7e1M0bkd0whprp=T0u=rvARSaPOGrY-NDQ@mail.gmail.com.
-- The bug was not in combocid handling, but the sequence of commands
-- is very similar to the previous case.
--
-- If the new tuple of an UPDATE needs toasting or doesn't fit on the
-- page, heap_update() temporarily releases the buffer lock.  Before
-- doing so, it marks the old tuple as locked, and that tripped an
-- assertion if the tuple was already locked in the same mode earlier
-- in the transaction, and the multixid also contained an aborted
-- updating transaction.  The assertion assumed that the multixid on
-- the tuple was lock-only, but didn't take the possibility of an
-- aborted updating XID into account.  That was fixed by making
-- MultiXactIdExpand() to remove the aborted XID in all cases, making
-- the assertion valid again.
CREATE TABLE testcase(
	id int PRIMARY KEY,
	val text
);
INSERT INTO testcase VALUES (1, '');
BEGIN;
-- Lock the row in the top transaction
SELECT id FROM testcase WHERE id = 1 FOR NO KEY UPDATE;

-- Update the row in a subtransaction, creating a multixid, and abort
-- the subtransaction.
SAVEPOINT subxact;
UPDATE testcase SET val = 'subxact' WHERE id = 1;
ROLLBACK TO SAVEPOINT subxact;
RELEASE SAVEPOINT subxact;

-- Update the row again in the same top-level transaction.  The new
-- value is large so that it needs to be toasted; that triggers the
-- codepath in heap_update() that first locks the tuple.  Thanks to
-- the FOR NO KEY UPDATE select earlier in the transaction, the tuple
-- is already locked.
UPDATE testcase SET val = repeat('x', 10000) WHERE id = 1;

-- Should return the updated tuple
SELECT id, length(val) FROM testcase;
COMMIT;

DROP TABLE testcase;
