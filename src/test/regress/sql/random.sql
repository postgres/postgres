--
-- RANDOM
-- Test the random function
--

-- count the number of tuples originally, should be 1000
SELECT count(*) FROM onek;

-- pick three random rows, they shouldn't match
(SELECT unique1 AS random
  FROM onek ORDER BY random() LIMIT 1)
INTERSECT
(SELECT unique1 AS random
  FROM onek ORDER BY random() LIMIT 1)
INTERSECT
(SELECT unique1 AS random
  FROM onek ORDER BY random() LIMIT 1);

-- count roughly 1/10 of the tuples
SELECT count(*) AS random INTO RANDOM_TBL
  FROM onek WHERE random() < 1.0/10;

-- select again, the count should be different
INSERT INTO RANDOM_TBL (random)
  SELECT count(*)
  FROM onek WHERE random() < 1.0/10;

-- select again, the count should be different
INSERT INTO RANDOM_TBL (random)
  SELECT count(*)
  FROM onek WHERE random() < 1.0/10;

-- select again, the count should be different
INSERT INTO RANDOM_TBL (random)
  SELECT count(*)
  FROM onek WHERE random() < 1.0/10;

-- now test that they are different counts
SELECT random, count(random) FROM RANDOM_TBL
  GROUP BY random HAVING count(random) > 3;

SELECT AVG(random) FROM RANDOM_TBL
  HAVING AVG(random) NOT BETWEEN 80 AND 120;

