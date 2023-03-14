--
-- RANDOM
-- Test random() and allies
--
-- Tests in this file may have a small probability of failure,
-- since we are dealing with randomness.  Try to keep the failure
-- risk for any one test case under 1e-9.
--

-- There should be no duplicates in 1000 random() values.
-- (Assuming 52 random bits in the float8 results, we could
-- take as many as 3000 values and still have less than 1e-9 chance
-- of failure, per https://en.wikipedia.org/wiki/Birthday_problem)
SELECT r, count(*)
FROM (SELECT random() r FROM generate_series(1, 1000)) ss
GROUP BY r HAVING count(*) > 1;

-- The range should be [0, 1).  We can expect that at least one out of 2000
-- random values is in the lowest or highest 1% of the range with failure
-- probability less than about 1e-9.

SELECT count(*) FILTER (WHERE r < 0 OR r >= 1) AS out_of_range,
       (count(*) FILTER (WHERE r < 0.01)) > 0 AS has_small,
       (count(*) FILTER (WHERE r > 0.99)) > 0 AS has_large
FROM (SELECT random() r FROM generate_series(1, 2000)) ss;

-- Check for uniform distribution using the Kolmogorov-Smirnov test.

CREATE FUNCTION ks_test_uniform_random()
RETURNS boolean AS
$$
DECLARE
  n int := 1000;        -- Number of samples
  c float8 := 1.94947;  -- Critical value for 99.9% confidence
  ok boolean;
BEGIN
  ok := (
    WITH samples AS (
      SELECT random() r FROM generate_series(1, n) ORDER BY 1
    ), indexed_samples AS (
      SELECT (row_number() OVER())-1.0 i, r FROM samples
    )
    SELECT max(abs(i/n-r)) < c / sqrt(n) FROM indexed_samples
  );
  RETURN ok;
END
$$
LANGUAGE plpgsql;

-- As written, ks_test_uniform_random() returns true about 99.9%
-- of the time.  To get down to a roughly 1e-9 test failure rate,
-- just run it 3 times and accept if any one of them passes.
SELECT ks_test_uniform_random() OR
       ks_test_uniform_random() OR
       ks_test_uniform_random() AS uniform;

-- now test random_normal()

-- As above, there should be no duplicates in 1000 random_normal() values.
SELECT r, count(*)
FROM (SELECT random_normal() r FROM generate_series(1, 1000)) ss
GROUP BY r HAVING count(*) > 1;

-- ... unless we force the range (standard deviation) to zero.
-- This is a good place to check that the mean input does something, too.
SELECT r, count(*)
FROM (SELECT random_normal(10, 0) r FROM generate_series(1, 100)) ss
GROUP BY r;
SELECT r, count(*)
FROM (SELECT random_normal(-10, 0) r FROM generate_series(1, 100)) ss
GROUP BY r;

-- Check standard normal distribution using the Kolmogorov-Smirnov test.

CREATE FUNCTION ks_test_normal_random()
RETURNS boolean AS
$$
DECLARE
  n int := 1000;        -- Number of samples
  c float8 := 1.94947;  -- Critical value for 99.9% confidence
  ok boolean;
BEGIN
  ok := (
    WITH samples AS (
      SELECT random_normal() r FROM generate_series(1, n) ORDER BY 1
    ), indexed_samples AS (
      SELECT (row_number() OVER())-1.0 i, r FROM samples
    )
    SELECT max(abs((1+erf(r/sqrt(2)))/2 - i/n)) < c / sqrt(n)
    FROM indexed_samples
  );
  RETURN ok;
END
$$
LANGUAGE plpgsql;

-- As above, ks_test_normal_random() returns true about 99.9%
-- of the time, so try it 3 times and accept if any test passes.
SELECT ks_test_normal_random() OR
       ks_test_normal_random() OR
       ks_test_normal_random() AS standard_normal;

-- setseed() should produce a reproducible series of random() values.

SELECT setseed(0.5);

SELECT random() FROM generate_series(1, 10);

-- Likewise for random_normal(); however, since its implementation relies
-- on libm functions that have different roundoff behaviors on different
-- machines, we have to round off the results a bit to get consistent output.
SET extra_float_digits = -1;

SELECT random_normal() FROM generate_series(1, 10);
SELECT random_normal(mean => 1, stddev => 0.1) r FROM generate_series(1, 10);
