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

