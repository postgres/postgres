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
