--
-- SELECT_DISTINCT
--

--
-- awk '{print $3;}' onek.data | sort -n | uniq
--
SELECT DISTINCT two FROM tmp ORDER BY 1;

--
-- awk '{print $5;}' onek.data | sort -n | uniq
--
SELECT DISTINCT ten FROM tmp ORDER BY 1;

--
-- awk '{print $16;}' onek.data | sort -d | uniq
--
SELECT DISTINCT string4 FROM tmp ORDER BY 1;

--
-- awk '{print $3,$16,$5;}' onek.data | sort -d | uniq |
-- sort +0n -1 +1d -2 +2n -3
--
SELECT DISTINCT two, string4, ten
   FROM tmp
   ORDER BY two using <, string4 using <, ten using <;

--
-- awk '{print $2;}' person.data |
-- awk '{if(NF!=1){print $2;}else{print;}}' - emp.data |
-- awk '{if(NF!=1){print $2;}else{print;}}' - student.data |
-- awk 'BEGIN{FS="      ";}{if(NF!=1){print $5;}else{print;}}' - stud_emp.data |
-- sort -n -r | uniq
--
SELECT DISTINCT p.age FROM person* p ORDER BY age using >;

--
-- Check mentioning same column more than once
--

EXPLAIN (VERBOSE, COSTS OFF)
SELECT count(*) FROM
  (SELECT DISTINCT two, four, two FROM tenk1) ss;

SELECT count(*) FROM
  (SELECT DISTINCT two, four, two FROM tenk1) ss;

--
-- Compare results between plans using sorting and plans using hash
-- aggregation. Force spilling in both cases by setting work_mem low.
--

SET work_mem='64kB';

-- Produce results with sorting.

SET enable_hashagg=FALSE;

SET jit_above_cost=0;

EXPLAIN (costs off)
SELECT DISTINCT g%1000 FROM generate_series(0,9999) g;

CREATE TABLE distinct_group_1 AS
SELECT DISTINCT g%1000 FROM generate_series(0,9999) g;

SET jit_above_cost TO DEFAULT;

CREATE TABLE distinct_group_2 AS
SELECT DISTINCT (g%1000)::text FROM generate_series(0,9999) g;

SET enable_hashagg=TRUE;

-- Produce results with hash aggregation.

SET enable_sort=FALSE;

SET jit_above_cost=0;

EXPLAIN (costs off)
SELECT DISTINCT g%1000 FROM generate_series(0,9999) g;

CREATE TABLE distinct_hash_1 AS
SELECT DISTINCT g%1000 FROM generate_series(0,9999) g;

SET jit_above_cost TO DEFAULT;

CREATE TABLE distinct_hash_2 AS
SELECT DISTINCT (g%1000)::text FROM generate_series(0,9999) g;

SET enable_sort=TRUE;

SET work_mem TO DEFAULT;

-- Compare results

(SELECT * FROM distinct_hash_1 EXCEPT SELECT * FROM distinct_group_1)
  UNION ALL
(SELECT * FROM distinct_group_1 EXCEPT SELECT * FROM distinct_hash_1);

(SELECT * FROM distinct_hash_1 EXCEPT SELECT * FROM distinct_group_1)
  UNION ALL
(SELECT * FROM distinct_group_1 EXCEPT SELECT * FROM distinct_hash_1);

DROP TABLE distinct_hash_1;
DROP TABLE distinct_hash_2;
DROP TABLE distinct_group_1;
DROP TABLE distinct_group_2;

--
-- Also, some tests of IS DISTINCT FROM, which doesn't quite deserve its
-- very own regression file.
--

CREATE TEMP TABLE disttable (f1 integer);
INSERT INTO DISTTABLE VALUES(1);
INSERT INTO DISTTABLE VALUES(2);
INSERT INTO DISTTABLE VALUES(3);
INSERT INTO DISTTABLE VALUES(NULL);

-- basic cases
SELECT f1, f1 IS DISTINCT FROM 2 as "not 2" FROM disttable;
SELECT f1, f1 IS DISTINCT FROM NULL as "not null" FROM disttable;
SELECT f1, f1 IS DISTINCT FROM f1 as "false" FROM disttable;
SELECT f1, f1 IS DISTINCT FROM f1+1 as "not null" FROM disttable;

-- check that optimizer constant-folds it properly
SELECT 1 IS DISTINCT FROM 2 as "yes";
SELECT 2 IS DISTINCT FROM 2 as "no";
SELECT 2 IS DISTINCT FROM null as "yes";
SELECT null IS DISTINCT FROM null as "no";

-- negated form
SELECT 1 IS NOT DISTINCT FROM 2 as "no";
SELECT 2 IS NOT DISTINCT FROM 2 as "yes";
SELECT 2 IS NOT DISTINCT FROM null as "no";
SELECT null IS NOT DISTINCT FROM null as "yes";
