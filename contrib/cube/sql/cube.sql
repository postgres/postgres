--
--  Test cube datatype
--

--
-- first, define the datatype.  Turn off echoing so that expected file
-- does not depend on contents of cube.sql.
--
\set ECHO none
\i cube.sql
\set ECHO all

--
-- testing the input and output functions
--

-- Any number (a one-dimensional point)
SELECT '1'::cube AS cube;
SELECT '-1'::cube AS cube;
SELECT '1.'::cube AS cube;
SELECT '-1.'::cube AS cube;
SELECT '.1'::cube AS cube;
SELECT '-.1'::cube AS cube;
SELECT '1.0'::cube AS cube;
SELECT '-1.0'::cube AS cube;
SELECT '1e7'::cube AS cube;
SELECT '-1e7'::cube AS cube;
SELECT '1.0e7'::cube AS cube;
SELECT '-1.0e7'::cube AS cube;
SELECT '1e+7'::cube AS cube;
SELECT '-1e+7'::cube AS cube;
SELECT '1.0e+7'::cube AS cube;
SELECT '-1.0e+7'::cube AS cube;
SELECT '1e-7'::cube AS cube;
SELECT '-1e-7'::cube AS cube;
SELECT '1.0e-7'::cube AS cube;
SELECT '-1.0e-7'::cube AS cube;
SELECT '1e700'::cube AS cube;
SELECT '-1e700'::cube AS cube;
SELECT '1e-700'::cube AS cube;
SELECT '-1e-700'::cube AS cube;

-- simple lists (points)
SELECT '1,2'::cube AS cube;
SELECT '(1,2)'::cube AS cube;
SELECT '1,2,3,4,5'::cube AS cube;
SELECT '(1,2,3,4,5)'::cube AS cube;

-- double lists (cubes)
SELECT '(0),(0)'::cube AS cube;
SELECT '(0),(1)'::cube AS cube;
SELECT '[(0),(0)]'::cube AS cube;
SELECT '[(0),(1)]'::cube AS cube;
SELECT '(0,0,0,0),(0,0,0,0)'::cube AS cube;
SELECT '(0,0,0,0),(1,0,0,0)'::cube AS cube;
SELECT '[(0,0,0,0),(0,0,0,0)]'::cube AS cube;
SELECT '[(0,0,0,0),(1,0,0,0)]'::cube AS cube;

-- invalid input: parse errors
SELECT ''::cube AS cube;
SELECT 'ABC'::cube AS cube;
SELECT '()'::cube AS cube;
SELECT '[]'::cube AS cube;
SELECT '[()]'::cube AS cube;
SELECT '[(1)]'::cube AS cube;
SELECT '[(1),]'::cube AS cube;
SELECT '[(1),2]'::cube AS cube;
SELECT '[(1),(2),(3)]'::cube AS cube;
SELECT '1,'::cube AS cube;
SELECT '1,2,'::cube AS cube;
SELECT '1,,2'::cube AS cube;
SELECT '(1,)'::cube AS cube;
SELECT '(1,2,)'::cube AS cube;
SELECT '(1,,2)'::cube AS cube;

-- invalid input: semantic errors and trailing garbage
SELECT '[(1),(2)],'::cube AS cube; -- 0
SELECT '[(1,2,3),(2,3)]'::cube AS cube; -- 1
SELECT '[(1,2),(1,2,3)]'::cube AS cube; -- 1
SELECT '(1),(2),'::cube AS cube; -- 2
SELECT '(1,2,3),(2,3)'::cube AS cube; -- 3
SELECT '(1,2),(1,2,3)'::cube AS cube; -- 3
SELECT '(1,2,3)ab'::cube AS cube; -- 4
SELECT '(1,2,3)a'::cube AS cube; -- 5
SELECT '(1,2)('::cube AS cube; -- 5
SELECT '1,2ab'::cube AS cube; -- 6
SELECT '1 e7'::cube AS cube; -- 6
SELECT '1,2a'::cube AS cube; -- 7
SELECT '1..2'::cube AS cube; -- 7

--
-- testing the  operators
--

-- equality/inequality:
--
SELECT '24, 33.20'::cube    =  '24, 33.20'::cube AS bool;
SELECT '24, 33.20'::cube    != '24, 33.20'::cube AS bool;
SELECT '24, 33.20'::cube    =  '24, 33.21'::cube AS bool;
SELECT '24, 33.20'::cube    != '24, 33.21'::cube AS bool;
SELECT '(2,0),(3,1)'::cube  =  '(2,0,0,0,0),(3,1,0,0,0)'::cube AS bool;
SELECT '(2,0),(3,1)'::cube  =  '(2,0,0,0,0),(3,1,0,0,1)'::cube AS bool;

-- "lower than" / "greater than"
-- (these operators are not useful for anything but ordering)
--
SELECT '1'::cube   > '2'::cube AS bool;
SELECT '1'::cube   < '2'::cube AS bool;
SELECT '1,1'::cube > '1,2'::cube AS bool;
SELECT '1,1'::cube < '1,2'::cube AS bool;

SELECT '(2,0),(3,1)'::cube             > '(2,0,0,0,0),(3,1,0,0,1)'::cube AS bool;
SELECT '(2,0),(3,1)'::cube             < '(2,0,0,0,0),(3,1,0,0,1)'::cube AS bool;
SELECT '(2,0),(3,1)'::cube             > '(2,0,0,0,1),(3,1,0,0,0)'::cube AS bool;
SELECT '(2,0),(3,1)'::cube             < '(2,0,0,0,1),(3,1,0,0,0)'::cube AS bool;
SELECT '(2,0),(3,1)'::cube             > '(2,0,0,0,0),(3,1,0,0,0)'::cube AS bool;
SELECT '(2,0),(3,1)'::cube             < '(2,0,0,0,0),(3,1,0,0,0)'::cube AS bool;
SELECT '(2,0,0,0,0),(3,1,0,0,1)'::cube > '(2,0),(3,1)'::cube AS bool;
SELECT '(2,0,0,0,0),(3,1,0,0,1)'::cube < '(2,0),(3,1)'::cube AS bool;
SELECT '(2,0,0,0,1),(3,1,0,0,0)'::cube > '(2,0),(3,1)'::cube AS bool;
SELECT '(2,0,0,0,1),(3,1,0,0,0)'::cube < '(2,0),(3,1)'::cube AS bool;
SELECT '(2,0,0,0,0),(3,1,0,0,0)'::cube > '(2,0),(3,1)'::cube AS bool;
SELECT '(2,0,0,0,0),(3,1,0,0,0)'::cube < '(2,0),(3,1)'::cube AS bool;


-- "overlap"
--
SELECT '1'::cube && '1'::cube AS bool;
SELECT '1'::cube && '2'::cube AS bool;

SELECT '[(-1,-1,-1),(1,1,1)]'::cube && '0'::cube AS bool;
SELECT '[(-1,-1,-1),(1,1,1)]'::cube && '1'::cube AS bool;
SELECT '[(-1,-1,-1),(1,1,1)]'::cube && '1,1,1'::cube AS bool;
SELECT '[(-1,-1,-1),(1,1,1)]'::cube && '[(1,1,1),(2,2,2)]'::cube AS bool;
SELECT '[(-1,-1,-1),(1,1,1)]'::cube && '[(1,1),(2,2)]'::cube AS bool;
SELECT '[(-1,-1,-1),(1,1,1)]'::cube && '[(2,1,1),(2,2,2)]'::cube AS bool;

-- "overlap on the left" / "overlap on the right"
-- (these operators are not useful at all but R-tree seems to be
-- sensitive to their presence)
--
SELECT '1'::cube &< '0'::cube AS bool;
SELECT '1'::cube &< '1'::cube AS bool;
SELECT '1'::cube &< '2'::cube AS bool;

SELECT '(0),(1)'::cube &< '0'::cube AS bool;
SELECT '(0),(1)'::cube &< '1'::cube AS bool;
SELECT '(0),(1)'::cube &< '(0),(0.5)'::cube AS bool;
SELECT '(0),(1)'::cube &< '(0),(1)'::cube AS bool;
SELECT '(0),(1)'::cube &< '(0),(2)'::cube AS bool;
SELECT '(0),(1)'::cube &< '(1),(2)'::cube AS bool;
SELECT '(0),(1)'::cube &< '(2),(3)'::cube AS bool;

SELECT '0'::cube &> '1'::cube AS bool;
SELECT '1'::cube &> '1'::cube AS bool;
SELECT '2'::cube &> '1'::cube AS bool;

SELECT '0'::cube        &> '(0),(1)'::cube AS bool;
SELECT '1'::cube        &> '(0),(1)'::cube AS bool;
SELECT '(0),(0.5)'      &> '(0),(1)'::cube AS bool;
SELECT '(0),(1)'::cube  &> '(0),(1)'::cube AS bool;
SELECT '(0),(2)'::cube  &> '(0),(1)'::cube AS bool;
SELECT '(1),(2)'::cube  &> '(0),(1)'::cube AS bool;
SELECT '(2),(3)'::cube  &> '(0),(1)'::cube AS bool;


-- "left" / "right"
-- (these operators are not useful but for 1-D or 2-D cubes, but R-tree
-- seems to want them defined)
--
SELECT '1'::cube << '0'::cube AS bool;
SELECT '1'::cube << '1'::cube AS bool;
SELECT '1'::cube << '2'::cube AS bool;

SELECT '(0),(1)'::cube << '0'::cube AS bool;
SELECT '(0),(1)'::cube << '1'::cube AS bool;
SELECT '(0),(1)'::cube << '(0),(0.5)'::cube AS bool;
SELECT '(0),(1)'::cube << '(0),(1)'::cube AS bool;
SELECT '(0),(1)'::cube << '(0),(2)'::cube AS bool;
SELECT '(0),(1)'::cube << '(1),(2)'::cube AS bool;
SELECT '(0),(1)'::cube << '(2),(3)'::cube AS bool;

SELECT '0'::cube >> '1'::cube AS bool;
SELECT '1'::cube >> '1'::cube AS bool;
SELECT '2'::cube >> '1'::cube AS bool;

SELECT '0'::cube        >> '(0),(1)'::cube AS bool;
SELECT '1'::cube        >> '(0),(1)'::cube AS bool;
SELECT '(0),(0.5)'      >> '(0),(1)'::cube AS bool;
SELECT '(0),(1)'::cube  >> '(0),(1)'::cube AS bool;
SELECT '(0),(2)'::cube  >> '(0),(1)'::cube AS bool;
SELECT '(1),(2)'::cube  >> '(0),(1)'::cube AS bool;
SELECT '(2),(3)'::cube  >> '(0),(1)'::cube AS bool;


-- "contained in" (the left operand is the cube entirely enclosed by
-- the right operand):
--
SELECT '0'::cube                 ~ '0'::cube                        AS bool;
SELECT '0,0,0'::cube             ~ '0,0,0'::cube                    AS bool;
SELECT '0,0'::cube               ~ '0,0,1'::cube                    AS bool;
SELECT '0,0,0'::cube             ~ '0,0,1'::cube                    AS bool;
SELECT '1,0,0'::cube             ~ '0,0,1'::cube                    AS bool;
SELECT '(1,0,0),(0,0,1)'::cube   ~ '(1,0,0),(0,0,1)'::cube          AS bool;
SELECT '(1,0,0),(0,0,1)'::cube   ~ '(-1,-1,-1),(1,1,1)'::cube       AS bool;
SELECT '(1,0,0),(0,0,1)'::cube   ~ '(-1,-1,-1,-1),(1,1,1,1)'::cube  AS bool;
SELECT '0'::cube                 ~ '(-1),(1)'::cube                 AS bool;
SELECT '1'::cube                 ~ '(-1),(1)'::cube                 AS bool;
SELECT '-1'::cube                ~ '(-1),(1)'::cube                 AS bool;
SELECT '(-1),(1)'::cube          ~ '(-1),(1)'::cube                 AS bool;
SELECT '(-1),(1)'::cube          ~ '(-1,-1),(1,1)'::cube            AS bool;
SELECT '(-2),(1)'::cube          ~ '(-1),(1)'::cube                 AS bool;
SELECT '(-2),(1)'::cube          ~ '(-1,-1),(1,1)'::cube            AS bool;


-- "contains" (the left operand is the cube that entirely encloses the
-- right operand)
--
SELECT '0'::cube                        @ '0'::cube                 AS bool;
SELECT '0,0,0'::cube                    @ '0,0,0'::cube             AS bool;
SELECT '0,0,1'::cube                    @ '0,0'::cube               AS bool;
SELECT '0,0,1'::cube                    @ '0,0,0'::cube             AS bool;
SELECT '0,0,1'::cube                    @ '1,0,0'::cube             AS bool;
SELECT '(1,0,0),(0,0,1)'::cube          @ '(1,0,0),(0,0,1)'::cube   AS bool;
SELECT '(-1,-1,-1),(1,1,1)'::cube       @ '(1,0,0),(0,0,1)'::cube   AS bool;
SELECT '(-1,-1,-1,-1),(1,1,1,1)'::cube  @ '(1,0,0),(0,0,1)'::cube   AS bool;
SELECT '(-1),(1)'::cube                 @ '0'::cube                 AS bool;
SELECT '(-1),(1)'::cube                 @ '1'::cube                 AS bool;
SELECT '(-1),(1)'::cube                 @ '-1'::cube                AS bool;
SELECT '(-1),(1)'::cube                 @ '(-1),(1)'::cube          AS bool;
SELECT '(-1,-1),(1,1)'::cube            @ '(-1),(1)'::cube          AS bool;
SELECT '(-1),(1)'::cube                 @ '(-2),(1)'::cube          AS bool;
SELECT '(-1,-1),(1,1)'::cube            @ '(-2),(1)'::cube          AS bool;


-- Load some example data and build the index
-- 
CREATE TABLE test_cube (c cube);

\copy test_cube from 'data/test_cube.data'

CREATE INDEX test_cube_ix ON test_cube USING gist (c);
SELECT * FROM test_cube	WHERE c && '(3000,1000),(0,0)';

-- Test sorting 
SELECT * FROM test_cube	WHERE c && '(3000,1000),(0,0)' GROUP BY c;
