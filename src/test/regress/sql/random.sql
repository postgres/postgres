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
CREATE TABLE RANDOM_TBL AS
  SELECT count(*) AS random
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

-- now test random_normal()

TRUNCATE random_tbl;
INSERT INTO random_tbl (random)
  SELECT count(*)
  FROM onek WHERE random_normal(0, 1) < 0;
INSERT INTO random_tbl (random)
  SELECT count(*)
  FROM onek WHERE random_normal(0) < 0;
INSERT INTO random_tbl (random)
  SELECT count(*)
  FROM onek WHERE random_normal() < 0;
INSERT INTO random_tbl (random)
  SELECT count(*)
  FROM onek WHERE random_normal(stddev => 1, mean => 0) < 0;

-- expect similar, but not identical values
SELECT random, count(random) FROM random_tbl
  GROUP BY random HAVING count(random) > 3;

-- approximately check expected distribution
SELECT AVG(random) FROM random_tbl
  HAVING AVG(random) NOT BETWEEN 400 AND 600;
