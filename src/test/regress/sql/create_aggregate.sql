--
-- CREATE_AGGREGATE
--

-- all functions CREATEd
CREATE AGGREGATE newavg (
   sfunc1 = int4pl, basetype = int4, stype1 = int4, 
   sfunc2 = int4inc, stype2 = int4,
   finalfunc = int4div,
   initcond1 = '0', initcond2 = '0'
);

-- sfunc1 (value-dependent) only 
CREATE AGGREGATE newsum (
   sfunc1 = int4pl, basetype = int4, stype1 = int4, 
   initcond1 = '0'
);

-- sfunc2 (value-independent) only 
CREATE AGGREGATE newcnt (
   sfunc2 = int4inc, basetype = int4, stype2 = int4, 
   initcond2 = '0'
);

