--
-- RANDOM
-- Test the random function
--

-- count the number of tuples originally
SELECT count(*) FROM onek;

-- select roughly 1/10 of the tuples
-- Assume that the "onek" table has 1000 tuples
--  and try to bracket the correct number so we
--  have a regression test which can pass/fail
-- - thomas 1998-08-17
SELECT count(*) AS random INTO RANDOM_TBL
  FROM onek WHERE oidrand(onek.oid, 10);

-- select again, the count should be different
INSERT INTO RANDOM_TBL (random)
  SELECT count(*)
  FROM onek WHERE oidrand(onek.oid, 10);

-- now test the results for randomness in the correct range
SELECT random, count(random) FROM RANDOM_TBL
  GROUP BY random HAVING count(random) > 1;

SELECT random FROM RANDOM_TBL
  WHERE random NOT BETWEEN 80 AND 120;

