--
-- CREATE_AGGREGATE
--

-- all functions CREATEd
CREATE AGGREGATE newavg (
   sfunc = int4_accum, basetype = int4, stype = _numeric, 
   finalfunc = numeric_avg,
   initcond1 = '{0,0,0}'
);

-- test comments
COMMENT ON AGGREGATE newavg_wrong (int4) IS 'an agg comment';
COMMENT ON AGGREGATE newavg (int4) IS 'an agg comment';
COMMENT ON AGGREGATE newavg (int4) IS NULL;

-- without finalfunc; test obsolete spellings 'sfunc1' etc
CREATE AGGREGATE newsum (
   sfunc1 = int4pl, basetype = int4, stype1 = int4, 
   initcond1 = '0'
);

-- zero-argument aggregate
CREATE AGGREGATE newcnt (*) (
   sfunc = int8inc, stype = int8,
   initcond = '0'
);

-- old-style spelling of same
CREATE AGGREGATE oldcnt (
   sfunc = int8inc, basetype = 'ANY', stype = int8,
   initcond = '0'
);

-- aggregate that only cares about null/nonnull input
CREATE AGGREGATE newcnt ("any") (
   sfunc = int8inc_any, stype = int8,
   initcond = '0'
);

-- multi-argument aggregate
create function sum3(int8,int8,int8) returns int8 as
'select $1 + $2 + $3' language sql strict immutable;

create aggregate sum2(int8,int8) (
   sfunc = sum3, stype = int8,
   initcond = '0'
);

COMMENT ON AGGREGATE nosuchagg (*) IS 'should fail';
COMMENT ON AGGREGATE newcnt (*) IS 'an agg(*) comment';
COMMENT ON AGGREGATE newcnt ("any") IS 'an agg(any) comment';
