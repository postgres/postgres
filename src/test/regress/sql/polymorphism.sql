-- Currently this tests polymorphic aggregates and indirectly does some
-- testing of polymorphic SQL functions.  It ought to be extended.


-- Legend:
-----------
-- A = type is ANY
-- P = type is polymorphic
-- N = type is non-polymorphic
-- B = aggregate base type
-- S = aggregate state type
-- R = aggregate return type
-- 1 = arg1 of a function
-- 2 = arg2 of a function
-- ag = aggregate
-- tf = trans (state) function
-- ff = final function
-- rt = return type of a function
-- -> = implies
-- => = allowed
-- !> = not allowed
-- E  = exists
-- NE = not-exists
-- 
-- Possible states:
-- ----------------
-- B = (A || P || N)
--   when (B = A) -> (tf2 = NE)
-- S = (P || N)
-- ff = (E || NE)
-- tf1 = (P || N)
-- tf2 = (NE || P || N)
-- R = (P || N)

-- create functions for use as tf and ff with the needed combinations of
-- argument polymorphism, but within the constraints of valid aggregate
-- functions, i.e. tf arg1 and tf return type must match

-- polymorphic single arg transfn
CREATE FUNCTION stfp(anyarray) returns anyarray as
'select $1' language 'sql';
-- non-polymorphic single arg transfn
CREATE FUNCTION stfnp(int[]) returns int[] as
'select $1' language 'sql';

-- dual polymorphic transfn
CREATE FUNCTION tfp(anyarray,anyelement) returns anyarray as
'select $1 || $2' language 'sql';
-- dual non-polymorphic transfn
CREATE FUNCTION tfnp(int[],int) returns int[] as
'select $1 || $2' language 'sql';

-- arg1 only polymorphic transfn
CREATE FUNCTION tf1p(anyarray,int) returns anyarray as
'select $1' language 'sql';
-- arg2 only polymorphic transfn
CREATE FUNCTION tf2p(int[],anyelement) returns int[] as
'select $1' language 'sql';

-- finalfn polymorphic
CREATE FUNCTION ffp(anyarray) returns anyarray as
'select $1' language 'sql';
-- finalfn non-polymorphic
CREATE FUNCTION ffnp(int[]) returns int[] as
'select $1' language 'sql';

-- Try to cover all the possible states:
-- 
-- Note: in Cases 1 & 2, we are trying to return P. Therefore, if the transfn
-- is stfnp, tfnp, or tf2p, we must use ffp as finalfn, because stfnp, tfnp,
-- and tf2p do not return P. Conversely, in Cases 3 & 4, we are trying to
-- return N. Therefore, if the transfn is stfp, tfp, or tf1p, we must use ffnp
-- as finalfn, because stfp, tfp, and tf1p do not return N.
--
--     Case1 (R = P) && (B = A)
--     ------------------------
--     S    tf1
--     -------
--     N    N
-- should CREATE
CREATE AGGREGATE myaggp01a(BASETYPE = "ANY", SFUNC = stfnp, STYPE = int4[],
  FINALFUNC = ffp, INITCOND = '{}');

--     P    N
-- should ERROR: stfnp(anyarray) not matched by stfnp(int[])
CREATE AGGREGATE myaggp02a(BASETYPE = "ANY", SFUNC = stfnp, STYPE = anyarray,
  FINALFUNC = ffp, INITCOND = '{}');

--     N    P
-- should CREATE
CREATE AGGREGATE myaggp03a(BASETYPE = "ANY", SFUNC = stfp, STYPE = int4[],
  FINALFUNC = ffp, INITCOND = '{}');
CREATE AGGREGATE myaggp03b(BASETYPE = "ANY", SFUNC = stfp, STYPE = int4[],
  INITCOND = '{}');

--     P    P
-- should ERROR: we have no way to resolve S
CREATE AGGREGATE myaggp04a(BASETYPE = "ANY", SFUNC = stfp, STYPE = anyarray,
  FINALFUNC = ffp, INITCOND = '{}');
CREATE AGGREGATE myaggp04b(BASETYPE = "ANY", SFUNC = stfp, STYPE = anyarray,
  INITCOND = '{}');


--    Case2 (R = P) && ((B = P) || (B = N))
--    -------------------------------------
--    S    tf1      B    tf2
--    -----------------------
--    N    N        N    N
-- should CREATE
CREATE AGGREGATE myaggp05a(BASETYPE = int, SFUNC = tfnp, STYPE = int[],
  FINALFUNC = ffp, INITCOND = '{}');

--    N    N        N    P
-- should CREATE
CREATE AGGREGATE myaggp06a(BASETYPE = int, SFUNC = tf2p, STYPE = int[],
  FINALFUNC = ffp, INITCOND = '{}');

--    N    N        P    N
-- should ERROR: tfnp(int[], anyelement) not matched by tfnp(int[], int)
CREATE AGGREGATE myaggp07a(BASETYPE = anyelement, SFUNC = tfnp, STYPE = int[],
  FINALFUNC = ffp, INITCOND = '{}');

--    N    N        P    P
-- should CREATE
CREATE AGGREGATE myaggp08a(BASETYPE = anyelement, SFUNC = tf2p, STYPE = int[],
  FINALFUNC = ffp, INITCOND = '{}');

--    N    P        N    N
-- should CREATE
CREATE AGGREGATE myaggp09a(BASETYPE = int, SFUNC = tf1p, STYPE = int[],
  FINALFUNC = ffp, INITCOND = '{}');
CREATE AGGREGATE myaggp09b(BASETYPE = int, SFUNC = tf1p, STYPE = int[],
  INITCOND = '{}');

--    N    P        N    P
-- should CREATE
CREATE AGGREGATE myaggp10a(BASETYPE = int, SFUNC = tfp, STYPE = int[],
  FINALFUNC = ffp, INITCOND = '{}');
CREATE AGGREGATE myaggp10b(BASETYPE = int, SFUNC = tfp, STYPE = int[],
  INITCOND = '{}');

--    N    P        P    N
-- should ERROR: tf1p(int[],anyelement) not matched by tf1p(anyarray,int)
CREATE AGGREGATE myaggp11a(BASETYPE = anyelement, SFUNC = tf1p, STYPE = int[],
  FINALFUNC = ffp, INITCOND = '{}');
CREATE AGGREGATE myaggp11b(BASETYPE = anyelement, SFUNC = tf1p, STYPE = int[],
  INITCOND = '{}');

--    N    P        P    P
-- should ERROR: tfp(int[],anyelement) not matched by tfp(anyarray,anyelement)
CREATE AGGREGATE myaggp12a(BASETYPE = anyelement, SFUNC = tfp, STYPE = int[],
  FINALFUNC = ffp, INITCOND = '{}');
CREATE AGGREGATE myaggp12b(BASETYPE = anyelement, SFUNC = tfp, STYPE = int[],
  INITCOND = '{}');

--    P    N        N    N
-- should ERROR: tfnp(anyarray, int) not matched by tfnp(int[],int)
CREATE AGGREGATE myaggp13a(BASETYPE = int, SFUNC = tfnp, STYPE = anyarray,
  FINALFUNC = ffp, INITCOND = '{}');

--    P    N        N    P
-- should ERROR: tf2p(anyarray, int) not matched by tf2p(int[],anyelement)
CREATE AGGREGATE myaggp14a(BASETYPE = int, SFUNC = tf2p, STYPE = anyarray,
  FINALFUNC = ffp, INITCOND = '{}');

--    P    N        P    N
-- should ERROR: tfnp(anyarray, anyelement) not matched by tfnp(int[],int)
CREATE AGGREGATE myaggp15a(BASETYPE = anyelement, SFUNC = tfnp,
  STYPE = anyarray, FINALFUNC = ffp, INITCOND = '{}');

--    P    N        P    P
-- should ERROR: tf2p(anyarray, anyelement) not matched by tf2p(int[],anyelement)
CREATE AGGREGATE myaggp16a(BASETYPE = anyelement, SFUNC = tf2p,
  STYPE = anyarray, FINALFUNC = ffp, INITCOND = '{}');

--    P    P        N    N
-- should ERROR: we have no way to resolve S
CREATE AGGREGATE myaggp17a(BASETYPE = int, SFUNC = tf1p, STYPE = anyarray,
  FINALFUNC = ffp, INITCOND = '{}');
CREATE AGGREGATE myaggp17b(BASETYPE = int, SFUNC = tf1p, STYPE = anyarray,
  INITCOND = '{}');

--    P    P        N    P
-- should ERROR: tfp(anyarray, int) not matched by tfp(anyarray, anyelement)
CREATE AGGREGATE myaggp18a(BASETYPE = int, SFUNC = tfp, STYPE = anyarray,
  FINALFUNC = ffp, INITCOND = '{}');
CREATE AGGREGATE myaggp18b(BASETYPE = int, SFUNC = tfp, STYPE = anyarray,
  INITCOND = '{}');

--    P    P        P    N
-- should ERROR: tf1p(anyarray, anyelement) not matched by tf1p(anyarray, int)
CREATE AGGREGATE myaggp19a(BASETYPE = anyelement, SFUNC = tf1p,
  STYPE = anyarray, FINALFUNC = ffp, INITCOND = '{}');
CREATE AGGREGATE myaggp19b(BASETYPE = anyelement, SFUNC = tf1p,
  STYPE = anyarray, INITCOND = '{}');

--    P    P        P    P
-- should CREATE
CREATE AGGREGATE myaggp20a(BASETYPE = anyelement, SFUNC = tfp,
  STYPE = anyarray, FINALFUNC = ffp, INITCOND = '{}');
CREATE AGGREGATE myaggp20b(BASETYPE = anyelement, SFUNC = tfp,
  STYPE = anyarray, INITCOND = '{}');

--     Case3 (R = N) && (B = A)
--     ------------------------
--     S    tf1
--     -------
--     N    N
-- should CREATE
CREATE AGGREGATE myaggn01a(BASETYPE = "ANY", SFUNC = stfnp, STYPE = int4[],
  FINALFUNC = ffnp, INITCOND = '{}');
CREATE AGGREGATE myaggn01b(BASETYPE = "ANY", SFUNC = stfnp, STYPE = int4[],
  INITCOND = '{}');

--     P    N
-- should ERROR: stfnp(anyarray) not matched by stfnp(int[])
CREATE AGGREGATE myaggn02a(BASETYPE = "ANY", SFUNC = stfnp, STYPE = anyarray,
  FINALFUNC = ffnp, INITCOND = '{}');
CREATE AGGREGATE myaggn02b(BASETYPE = "ANY", SFUNC = stfnp, STYPE = anyarray,
  INITCOND = '{}');

--     N    P
-- should CREATE
CREATE AGGREGATE myaggn03a(BASETYPE = "ANY", SFUNC = stfp, STYPE = int4[],
  FINALFUNC = ffnp, INITCOND = '{}');

--     P    P
-- should ERROR: ffnp(anyarray) not matched by ffnp(int[])
CREATE AGGREGATE myaggn04a(BASETYPE = "ANY", SFUNC = stfp, STYPE = anyarray,
  FINALFUNC = ffnp, INITCOND = '{}');


--    Case4 (R = N) && ((B = P) || (B = N))
--    -------------------------------------
--    S    tf1      B    tf2
--    -----------------------
--    N    N        N    N
-- should CREATE
CREATE AGGREGATE myaggn05a(BASETYPE = int, SFUNC = tfnp, STYPE = int[],
  FINALFUNC = ffnp, INITCOND = '{}');
CREATE AGGREGATE myaggn05b(BASETYPE = int, SFUNC = tfnp, STYPE = int[],
  INITCOND = '{}');

--    N    N        N    P
-- should CREATE
CREATE AGGREGATE myaggn06a(BASETYPE = int, SFUNC = tf2p, STYPE = int[],
  FINALFUNC = ffnp, INITCOND = '{}');
CREATE AGGREGATE myaggn06b(BASETYPE = int, SFUNC = tf2p, STYPE = int[],
  INITCOND = '{}');

--    N    N        P    N
-- should ERROR: tfnp(int[], anyelement) not matched by tfnp(int[], int)
CREATE AGGREGATE myaggn07a(BASETYPE = anyelement, SFUNC = tfnp, STYPE = int[],
  FINALFUNC = ffnp, INITCOND = '{}');
CREATE AGGREGATE myaggn07b(BASETYPE = anyelement, SFUNC = tfnp, STYPE = int[],
  INITCOND = '{}');

--    N    N        P    P
-- should CREATE
CREATE AGGREGATE myaggn08a(BASETYPE = anyelement, SFUNC = tf2p, STYPE = int[],
  FINALFUNC = ffnp, INITCOND = '{}');
CREATE AGGREGATE myaggn08b(BASETYPE = anyelement, SFUNC = tf2p, STYPE = int[],
  INITCOND = '{}');

--    N    P        N    N
-- should CREATE
CREATE AGGREGATE myaggn09a(BASETYPE = int, SFUNC = tf1p, STYPE = int[],
  FINALFUNC = ffnp, INITCOND = '{}');

--    N    P        N    P
-- should CREATE
CREATE AGGREGATE myaggn10a(BASETYPE = int, SFUNC = tfp, STYPE = int[],
  FINALFUNC = ffnp, INITCOND = '{}');

--    N    P        P    N
-- should ERROR: tf1p(int[],anyelement) not matched by tf1p(anyarray,int)
CREATE AGGREGATE myaggn11a(BASETYPE = anyelement, SFUNC = tf1p, STYPE = int[],
  FINALFUNC = ffnp, INITCOND = '{}');

--    N    P        P    P
-- should ERROR: tfp(int[],anyelement) not matched by tfp(anyarray,anyelement)
CREATE AGGREGATE myaggn12a(BASETYPE = anyelement, SFUNC = tfp, STYPE = int[],
  FINALFUNC = ffnp, INITCOND = '{}');

--    P    N        N    N
-- should ERROR: tfnp(anyarray, int) not matched by tfnp(int[],int)
CREATE AGGREGATE myaggn13a(BASETYPE = int, SFUNC = tfnp, STYPE = anyarray,
  FINALFUNC = ffnp, INITCOND = '{}');
CREATE AGGREGATE myaggn13b(BASETYPE = int, SFUNC = tfnp, STYPE = anyarray,
  INITCOND = '{}');

--    P    N        N    P
-- should ERROR: tf2p(anyarray, int) not matched by tf2p(int[],anyelement)
CREATE AGGREGATE myaggn14a(BASETYPE = int, SFUNC = tf2p, STYPE = anyarray,
  FINALFUNC = ffnp, INITCOND = '{}');
CREATE AGGREGATE myaggn14b(BASETYPE = int, SFUNC = tf2p, STYPE = anyarray,
  INITCOND = '{}');

--    P    N        P    N
-- should ERROR: tfnp(anyarray, anyelement) not matched by tfnp(int[],int)
CREATE AGGREGATE myaggn15a(BASETYPE = anyelement, SFUNC = tfnp,
  STYPE = anyarray, FINALFUNC = ffnp, INITCOND = '{}');
CREATE AGGREGATE myaggn15b(BASETYPE = anyelement, SFUNC = tfnp,
  STYPE = anyarray, INITCOND = '{}');

--    P    N        P    P
-- should ERROR: tf2p(anyarray, anyelement) not matched by tf2p(int[],anyelement)
CREATE AGGREGATE myaggn16a(BASETYPE = anyelement, SFUNC = tf2p,
  STYPE = anyarray, FINALFUNC = ffnp, INITCOND = '{}');
CREATE AGGREGATE myaggn16b(BASETYPE = anyelement, SFUNC = tf2p,
  STYPE = anyarray, INITCOND = '{}');

--    P    P        N    N
-- should ERROR: ffnp(anyarray) not matched by ffnp(int[])
CREATE AGGREGATE myaggn17a(BASETYPE = int, SFUNC = tf1p, STYPE = anyarray,
  FINALFUNC = ffnp, INITCOND = '{}');

--    P    P        N    P
-- should ERROR: tfp(anyarray, int) not matched by tfp(anyarray, anyelement)
CREATE AGGREGATE myaggn18a(BASETYPE = int, SFUNC = tfp, STYPE = anyarray,
  FINALFUNC = ffnp, INITCOND = '{}');

--    P    P        P    N
-- should ERROR: tf1p(anyarray, anyelement) not matched by tf1p(anyarray, int)
CREATE AGGREGATE myaggn19a(BASETYPE = anyelement, SFUNC = tf1p,
  STYPE = anyarray, FINALFUNC = ffnp, INITCOND = '{}');

--    P    P        P    P
-- should ERROR: ffnp(anyarray) not matched by ffnp(int[])
CREATE AGGREGATE myaggn20a(BASETYPE = anyelement, SFUNC = tfp,
  STYPE = anyarray, FINALFUNC = ffnp, INITCOND = '{}');

-- create test data for polymorphic aggregates
create temp table t(f1 int, f2 int[], f3 text);
insert into t values(1,array[1],'a');
insert into t values(1,array[11],'b');
insert into t values(1,array[111],'c');
insert into t values(2,array[2],'a');
insert into t values(2,array[22],'b');
insert into t values(2,array[222],'c');
insert into t values(3,array[3],'a');
insert into t values(3,array[3],'b');

-- test the successfully created polymorphic aggregates
select f3, myaggp01a(*) from t group by f3;
select f3, myaggp03a(*) from t group by f3;
select f3, myaggp03b(*) from t group by f3;
select f3, myaggp05a(f1) from t group by f3;
select f3, myaggp06a(f1) from t group by f3;
select f3, myaggp08a(f1) from t group by f3;
select f3, myaggp09a(f1) from t group by f3;
select f3, myaggp09b(f1) from t group by f3;
select f3, myaggp10a(f1) from t group by f3;
select f3, myaggp10b(f1) from t group by f3;
select f3, myaggp20a(f1) from t group by f3;
select f3, myaggp20b(f1) from t group by f3;
select f3, myaggn01a(*) from t group by f3;
select f3, myaggn01b(*) from t group by f3;
select f3, myaggn03a(*) from t group by f3;
select f3, myaggn05a(f1) from t group by f3;
select f3, myaggn05b(f1) from t group by f3;
select f3, myaggn06a(f1) from t group by f3;
select f3, myaggn06b(f1) from t group by f3;
select f3, myaggn08a(f1) from t group by f3;
select f3, myaggn08b(f1) from t group by f3;
select f3, myaggn09a(f1) from t group by f3;
select f3, myaggn10a(f1) from t group by f3;
