-- Aggregate functions
-- Thomas Lockhart
-- This fixes the behavior of built-in aggregate functions avg() and sum().
-- Code tested on postgres95 v1.0.9, postgres v6.0, and postgres v6.1b-970315.
-- Original definitions return zero rather than null for empty set attributes.
-- Postgres source code says that null behavior for aggregates is not correct,
--  but does describe the correct behavior for pass-by-reference data types
--  if it is given null initial values (from pg_aggregate).
-- Note that pass-by-value data types (e.g. int4) require a simple source code
--  change in backend/executor/nodeAgg.c to avoid divide-by-zero results.
-- If this SQL update is applied without making the corresponding source code
--  patch, then floating point types will work correctly but integer types will
--  divide-by-zero.
-- If the source code patch is applied but this SQL update is not, then there
--  will be divide-by-zero results for floating point types.

-- For aggregate attributes, the correct behavior is as follows:
-- count(*) should return a count of all tuples, null or otherwise
-- count(col) should return a count of all non-null values, zero if none
-- avg(col), sum(col), etc should ignore null fields and return null if there
--  are no non-null inputs
-- Ref: the original Date book

update pg_aggregate set agginitval1=null
 where aggname = 'avg' or aggname = 'sum';

-- Geometric functions
-- Thomas Lockhart
-- This replaces the distance operator with one returning a floating point number.
-- The original operator 'pointdist' returned an integer.
-- There is no corresponding source code change required for this patch.

update pg_operator set oprresult = 701, oprcode = 'point_distance'::regproc
 where oprname = '<===>' and oprresult = 23;

-- Date functions
-- Thomas Lockhart
-- This fixes conflicting OIDs within the date and time declarations.

update pg_proc set oid = 1138::oid where proname = 'date_larger';
update pg_proc set oid = 1139::oid where proname = 'date_smaller';
update pg_proc set oid = 1140::oid where proname = 'date_mi';
update pg_proc set oid = 1141::oid where proname = 'date_pli';
update pg_proc set oid = 1142::oid where proname = 'date_mii';
update pg_proc set oid = 1143::oid where proname = 'timein';
update pg_proc set oid = 1144::oid where proname = 'timeout';
update pg_proc set oid = 1145::oid where proname = 'time_eq';
