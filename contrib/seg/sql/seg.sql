--
--  Test seg datatype
--

--
-- first, define the datatype.  Turn off echoing so that expected file
-- does not depend on contents of seg.sql.
--
\set ECHO none
\i seg.sql
\set ECHO all

--
-- testing the input and output functions
--

-- Any number
SELECT '1'::seg AS seg;
SELECT '-1'::seg AS seg;
SELECT '1.0'::seg AS seg;
SELECT '-1.0'::seg AS seg;
SELECT '1e7'::seg AS seg;
SELECT '-1e7'::seg AS seg;
SELECT '1.0e7'::seg AS seg;
SELECT '-1.0e7'::seg AS seg;
SELECT '1e+7'::seg AS seg;
SELECT '-1e+7'::seg AS seg;
SELECT '1.0e+7'::seg AS seg;
SELECT '-1.0e+7'::seg AS seg;
SELECT '1e-7'::seg AS seg;
SELECT '-1e-7'::seg AS seg;
SELECT '1.0e-7'::seg AS seg;
SELECT '-1.0e-7'::seg AS seg;
SELECT '2e-6'::seg AS seg;
SELECT '2e-5'::seg AS seg;
SELECT '2e-4'::seg AS seg;
SELECT '2e-3'::seg AS seg;
SELECT '2e-2'::seg AS seg;
SELECT '2e-1'::seg AS seg;
SELECT '2e-0'::seg AS seg;
SELECT '2e+0'::seg AS seg;
SELECT '2e+1'::seg AS seg;
SELECT '2e+2'::seg AS seg;
SELECT '2e+3'::seg AS seg;
SELECT '2e+4'::seg AS seg;
SELECT '2e+5'::seg AS seg;
SELECT '2e+6'::seg AS seg;


-- Significant digits preserved
SELECT '1'::seg AS seg;
SELECT '1.0'::seg AS seg;
SELECT '1.00'::seg AS seg;
SELECT '1.000'::seg AS seg;
SELECT '1.0000'::seg AS seg;
SELECT '1.00000'::seg AS seg;
SELECT '1.000000'::seg AS seg;
SELECT '0.000000120'::seg AS seg;
SELECT '3.400e5'::seg AS seg;

-- Digits truncated
SELECT '12.34567890123456'::seg AS seg;

-- Numbers with certainty indicators
SELECT '~6.5'::seg AS seg;
SELECT '<6.5'::seg AS seg;
SELECT '>6.5'::seg AS seg;
SELECT '~ 6.5'::seg AS seg;
SELECT '< 6.5'::seg AS seg;
SELECT '> 6.5'::seg AS seg;

-- Open intervals
SELECT '0..'::seg AS seg;
SELECT '0...'::seg AS seg;
SELECT '0 ..'::seg AS seg;
SELECT '0 ...'::seg AS seg;
SELECT '..0'::seg AS seg;
SELECT '...0'::seg AS seg;
SELECT '.. 0'::seg AS seg;
SELECT '... 0'::seg AS seg;

-- Finite intervals
SELECT '0 .. 1'::seg AS seg;
SELECT '-1 .. 0'::seg AS seg;
SELECT '-1 .. 1'::seg AS seg;

-- (+/-) intervals
SELECT '0(+-)1'::seg AS seg;
SELECT '0(+-)1.0'::seg AS seg;
SELECT '1.0(+-)0.005'::seg AS seg;
SELECT '101(+-)1'::seg AS seg;
-- incorrect number of significant digits in 99.0:
SELECT '100(+-)1'::seg AS seg;

-- invalid input
SELECT ''::seg AS seg;
SELECT 'ABC'::seg AS seg;
SELECT '1ABC'::seg AS seg;
SELECT '1.'::seg AS seg;
SELECT '1.....'::seg AS seg;
SELECT '.1'::seg AS seg;
SELECT '1..2.'::seg AS seg;
SELECT '1 e7'::seg AS seg;
SELECT '1e700'::seg AS seg;

--
-- testing the  operators
--

-- equality/inequality:
--
SELECT '24 .. 33.20'::seg = '24 .. 33.20'::seg AS bool;
SELECT '24 .. 33.20'::seg = '24 .. 33.21'::seg AS bool;
SELECT '24 .. 33.20'::seg != '24 .. 33.20'::seg AS bool;
SELECT '24 .. 33.20'::seg != '24 .. 33.21'::seg AS bool;

-- overlap
--
SELECT '1'::seg && '1'::seg AS bool;
SELECT '1'::seg && '2'::seg AS bool;
SELECT '0 ..'::seg && '0 ..'::seg AS bool;
SELECT '0 .. 1'::seg && '0 .. 1'::seg AS bool;
SELECT '..0'::seg && '0..'::seg AS bool;
SELECT '-1 .. 0.1'::seg && '0 .. 1'::seg AS bool;
SELECT '-1 .. 0'::seg && '0 .. 1'::seg AS bool;
SELECT '-1 .. -0.0001'::seg && '0 .. 1'::seg AS bool;
SELECT '0 ..'::seg && '1'::seg AS bool;
SELECT '0 .. 1'::seg && '1'::seg AS bool;
SELECT '0 .. 1'::seg && '2'::seg AS bool;
SELECT '0 .. 2'::seg && '1'::seg AS bool;
SELECT '1'::seg && '0 .. 1'::seg AS bool;
SELECT '2'::seg && '0 .. 1'::seg AS bool;
SELECT '1'::seg && '0 .. 2'::seg AS bool;

-- overlap on the left
--
SELECT '1'::seg &< '0'::seg AS bool;
SELECT '1'::seg &< '1'::seg AS bool;
SELECT '1'::seg &< '2'::seg AS bool;
SELECT '0 .. 1'::seg &< '0'::seg AS bool;
SELECT '0 .. 1'::seg &< '1'::seg AS bool;
SELECT '0 .. 1'::seg &< '2'::seg AS bool;
SELECT '0 .. 1'::seg &< '0 .. 0.5'::seg AS bool;
SELECT '0 .. 1'::seg &< '0 .. 1'::seg AS bool;
SELECT '0 .. 1'::seg &< '0 .. 2'::seg AS bool;
SELECT '0 .. 1'::seg &< '1 .. 2'::seg AS bool;
SELECT '0 .. 1'::seg &< '2 .. 3'::seg AS bool;

-- overlap on the right
--
SELECT '0'::seg &> '1'::seg AS bool;
SELECT '1'::seg &> '1'::seg AS bool;
SELECT '2'::seg &> '1'::seg AS bool;
SELECT '0'::seg &> '0 .. 1'::seg AS bool;
SELECT '1'::seg &> '0 .. 1'::seg AS bool;
SELECT '2'::seg &> '0 .. 1'::seg AS bool;
SELECT '0 .. 0.5'::seg &> '0 .. 1'::seg AS bool;
SELECT '0 .. 1'::seg &> '0 .. 1'::seg AS bool;
SELECT '0 .. 2'::seg &> '0 .. 2'::seg AS bool;
SELECT '1 .. 2'::seg &> '0 .. 1'::seg AS bool;
SELECT '2 .. 3'::seg &> '0 .. 1'::seg AS bool;

-- left
--
SELECT '1'::seg << '0'::seg AS bool;
SELECT '1'::seg << '1'::seg AS bool;
SELECT '1'::seg << '2'::seg AS bool;
SELECT '0 .. 1'::seg << '0'::seg AS bool;
SELECT '0 .. 1'::seg << '1'::seg AS bool;
SELECT '0 .. 1'::seg << '2'::seg AS bool;
SELECT '0 .. 1'::seg << '0 .. 0.5'::seg AS bool;
SELECT '0 .. 1'::seg << '0 .. 1'::seg AS bool;
SELECT '0 .. 1'::seg << '0 .. 2'::seg AS bool;
SELECT '0 .. 1'::seg << '1 .. 2'::seg AS bool;
SELECT '0 .. 1'::seg << '2 .. 3'::seg AS bool;

-- right
--
SELECT '0'::seg >> '1'::seg AS bool;
SELECT '1'::seg >> '1'::seg AS bool;
SELECT '2'::seg >> '1'::seg AS bool;
SELECT '0'::seg >> '0 .. 1'::seg AS bool;
SELECT '1'::seg >> '0 .. 1'::seg AS bool;
SELECT '2'::seg >> '0 .. 1'::seg AS bool;
SELECT '0 .. 0.5'::seg >> '0 .. 1'::seg AS bool;
SELECT '0 .. 1'::seg >> '0 .. 1'::seg AS bool;
SELECT '0 .. 2'::seg >> '0 .. 2'::seg AS bool;
SELECT '1 .. 2'::seg >> '0 .. 1'::seg AS bool;
SELECT '2 .. 3'::seg >> '0 .. 1'::seg AS bool;


-- "contained in" (the left value belongs within the interval specified in the right value):
--
SELECT '0'::seg        ~ '0'::seg AS bool;
SELECT '0'::seg        ~ '0 ..'::seg AS bool;
SELECT '0'::seg        ~ '.. 0'::seg AS bool;
SELECT '0'::seg        ~ '-1 .. 1'::seg AS bool;
SELECT '0'::seg        ~ '-1 .. 1'::seg AS bool;
SELECT '-1'::seg       ~ '-1 .. 1'::seg AS bool;
SELECT '1'::seg        ~ '-1 .. 1'::seg AS bool;
SELECT '-1 .. 1'::seg  ~ '-1 .. 1'::seg AS bool;

-- "contains" (the left value contains the interval specified in the right value):
--
SELECT '0'::seg @ '0'::seg AS bool;
SELECT '0 .. '::seg ~ '0'::seg AS bool;
SELECT '.. 0'::seg ~ '0'::seg AS bool;
SELECT '-1 .. 1'::seg ~ '0'::seg AS bool;
SELECT '0'::seg ~ '-1 .. 1'::seg AS bool;
SELECT '-1'::seg ~ '-1 .. 1'::seg AS bool;
SELECT '1'::seg ~ '-1 .. 1'::seg AS bool;

-- Load some example data and build the index
-- 
CREATE TABLE test_seg (s seg);

\copy test_seg from 'data/test_seg.data'

CREATE INDEX test_seg_ix ON test_seg USING gist (s);
SELECT count(*) FROM test_seg WHERE s @ '11..11.3';

-- Test sorting 
SELECT * FROM test_seg WHERE s @ '11..11.3' GROUP BY s;
