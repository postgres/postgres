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

-- value-independent transition function
CREATE AGGREGATE newcnt (
   sfunc = int4inc, basetype = 'any', stype = int4,
   initcond = '0'
);

COMMENT ON AGGREGATE nosuchagg (*) IS 'should fail';
COMMENT ON AGGREGATE newcnt (*) IS 'an any agg comment';
COMMENT ON AGGREGATE newcnt (*) IS NULL;
