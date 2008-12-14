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
CREATE FUNCTION stfp(anyarray) RETURNS anyarray AS
'select $1' LANGUAGE SQL;
-- non-polymorphic single arg transfn
CREATE FUNCTION stfnp(int[]) RETURNS int[] AS
'select $1' LANGUAGE SQL;

-- dual polymorphic transfn
CREATE FUNCTION tfp(anyarray,anyelement) RETURNS anyarray AS
'select $1 || $2' LANGUAGE SQL;
-- dual non-polymorphic transfn
CREATE FUNCTION tfnp(int[],int) RETURNS int[] AS
'select $1 || $2' LANGUAGE SQL;

-- arg1 only polymorphic transfn
CREATE FUNCTION tf1p(anyarray,int) RETURNS anyarray AS
'select $1' LANGUAGE SQL;
-- arg2 only polymorphic transfn
CREATE FUNCTION tf2p(int[],anyelement) RETURNS int[] AS
'select $1' LANGUAGE SQL;

-- multi-arg polymorphic
CREATE FUNCTION sum3(anyelement,anyelement,anyelement) returns anyelement AS
'select $1+$2+$3' language sql strict;

-- finalfn polymorphic
CREATE FUNCTION ffp(anyarray) RETURNS anyarray AS
'select $1' LANGUAGE SQL;
-- finalfn non-polymorphic
CREATE FUNCTION ffnp(int[]) returns int[] as
'select $1' LANGUAGE SQL;

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
CREATE AGGREGATE myaggp01a(*) (SFUNC = stfnp, STYPE = int4[],
  FINALFUNC = ffp, INITCOND = '{}');

--     P    N
-- should ERROR: stfnp(anyarray) not matched by stfnp(int[])
CREATE AGGREGATE myaggp02a(*) (SFUNC = stfnp, STYPE = anyarray,
  FINALFUNC = ffp, INITCOND = '{}');

--     N    P
-- should CREATE
CREATE AGGREGATE myaggp03a(*) (SFUNC = stfp, STYPE = int4[],
  FINALFUNC = ffp, INITCOND = '{}');
CREATE AGGREGATE myaggp03b(*) (SFUNC = stfp, STYPE = int4[],
  INITCOND = '{}');

--     P    P
-- should ERROR: we have no way to resolve S
CREATE AGGREGATE myaggp04a(*) (SFUNC = stfp, STYPE = anyarray,
  FINALFUNC = ffp, INITCOND = '{}');
CREATE AGGREGATE myaggp04b(*) (SFUNC = stfp, STYPE = anyarray,
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
CREATE AGGREGATE myaggn01a(*) (SFUNC = stfnp, STYPE = int4[],
  FINALFUNC = ffnp, INITCOND = '{}');
CREATE AGGREGATE myaggn01b(*) (SFUNC = stfnp, STYPE = int4[],
  INITCOND = '{}');

--     P    N
-- should ERROR: stfnp(anyarray) not matched by stfnp(int[])
CREATE AGGREGATE myaggn02a(*) (SFUNC = stfnp, STYPE = anyarray,
  FINALFUNC = ffnp, INITCOND = '{}');
CREATE AGGREGATE myaggn02b(*) (SFUNC = stfnp, STYPE = anyarray,
  INITCOND = '{}');

--     N    P
-- should CREATE
CREATE AGGREGATE myaggn03a(*) (SFUNC = stfp, STYPE = int4[],
  FINALFUNC = ffnp, INITCOND = '{}');

--     P    P
-- should ERROR: ffnp(anyarray) not matched by ffnp(int[])
CREATE AGGREGATE myaggn04a(*) (SFUNC = stfp, STYPE = anyarray,
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

-- multi-arg polymorphic
CREATE AGGREGATE mysum2(anyelement,anyelement) (SFUNC = sum3,
  STYPE = anyelement, INITCOND = '0');

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
select mysum2(f1, f1 + 1) from t;

-- test inlining of polymorphic SQL functions
create function bleat(int) returns int as $$
begin
  raise notice 'bleat %', $1;
  return $1;
end$$ language plpgsql;

create function sql_if(bool, anyelement, anyelement) returns anyelement as $$
select case when $1 then $2 else $3 end $$ language sql;

-- Note this would fail with integer overflow, never mind wrong bleat() output,
-- if the CASE expression were not successfully inlined
select f1, sql_if(f1 > 0, bleat(f1), bleat(f1 + 1)) from int4_tbl;

select q2, sql_if(q2 > 0, q2, q2 + 1) from int8_tbl;

-- another kind of polymorphic aggregate

create function add_group(grp anyarray, ad anyelement, size integer)
  returns anyarray
  as $$
begin
  if grp is null then
    return array[ad];
  end if;
  if array_upper(grp, 1) < size then
    return grp || ad;
  end if;
  return grp;
end;
$$
  language plpgsql immutable;

create aggregate build_group(anyelement, integer) (
  SFUNC = add_group,
  STYPE = anyarray
);

select build_group(q1,3) from int8_tbl;

-- this should fail because stype isn't compatible with arg
create aggregate build_group(int8, integer) (
  SFUNC = add_group,
  STYPE = int2[]
);

-- but we can make a non-poly agg from a poly sfunc if types are OK
create aggregate build_group(int8, integer) (
  SFUNC = add_group,
  STYPE = int8[]
);

-- check that we can apply functions taking ANYARRAY to pg_stats
select distinct array_ndims(histogram_bounds) from pg_stats
where histogram_bounds is not null;

-- such functions must protect themselves if varying element type isn't OK
select max(histogram_bounds) from pg_stats;

-- test variadic polymorphic functions

create function myleast(variadic anyarray) returns anyelement as $$
  select min($1[i]) from generate_subscripts($1,1) g(i)
$$ language sql immutable strict;

select myleast(10, 1, 20, 33);
select myleast(1.1, 0.22, 0.55);
select myleast('z'::text);
select myleast(); -- fail

-- test with variadic call parameter
select myleast(variadic array[1,2,3,4,-1]);
select myleast(variadic array[1.1, -5.5]);

--test with empty variadic call parameter
select myleast(variadic array[]::int[]);

-- an example with some ordinary arguments too
create function concat(text, variadic anyarray) returns text as $$
  select array_to_string($2, $1);
$$ language sql immutable strict;

select concat('%', 1, 2, 3, 4, 5);
select concat('|', 'a'::text, 'b', 'c');
select concat('|', variadic array[1,2,33]);
select concat('|', variadic array[]::int[]);

drop function concat(text, anyarray);

-- mix variadic with anyelement
create function formarray(anyelement, variadic anyarray) returns anyarray as $$
  select array_prepend($1, $2);
$$ language sql immutable strict;

select formarray(1,2,3,4,5);
select formarray(1.1, variadic array[1.2,55.5]);
select formarray(1.1, array[1.2,55.5]); -- fail without variadic
select formarray(1, 'x'::text); -- fail, type mismatch
select formarray(1, variadic array['x'::text]); -- fail, type mismatch

drop function formarray(anyelement, variadic anyarray);

-- test pg_typeof() function
select pg_typeof(null);           -- unknown
select pg_typeof(0);              -- integer
select pg_typeof(0.0);            -- numeric
select pg_typeof(1+1 = 2);        -- boolean
select pg_typeof('x');            -- unknown
select pg_typeof('' || '');       -- text
select pg_typeof(pg_typeof(0));   -- regtype
select pg_typeof(array[1.2,55.5]); -- numeric[]
select pg_typeof(myleast(10, 1, 20, 33));  -- polymorphic input

-- test functions with parameter defaults
-- test basic functionality
create function dfunc(a int = 1, int = 2) returns int as $$
  select $1 + $2;
$$ language sql;

select dfunc();
select dfunc(10);
select dfunc(10, 20);

drop function dfunc();  -- fail
drop function dfunc(int);  -- fail
drop function dfunc(int, int);  -- ok

-- fail, gap in arguments with defaults
create function dfunc(a int = 1, b int) returns int as $$
  select $1 + $2;
$$ language sql;

-- check implicit coercion 
create function dfunc(a int DEFAULT 1.0, int DEFAULT '-1') returns int as $$
  select $1 + $2;
$$ language sql;
select dfunc();
create function dfunc(a text DEFAULT 'Hello', b text DEFAULT 'World') returns text as $$
  select $1 || ', ' || $2;
$$ language sql;

select dfunc();  -- fail; which dfunc should be called? int or text
select dfunc('Hi');  -- ok
select dfunc('Hi', 'City');  -- ok
select dfunc(0);  -- ok
select dfunc(10, 20);  -- ok

drop function dfunc(int, int);
drop function dfunc(text, text);

create function dfunc(int = 1, int = 2) returns int as $$
  select 2; 
$$ language sql;

create function dfunc(int = 1, int = 2, int = 3, int = 4) returns int as $$
  select 4;
$$ language sql;

-- Now, dfunc(nargs = 2) and dfunc(nargs = 4) are ambiguous when called
-- with 0 or 1 arguments.  For 2 arguments, a normall call of
-- dfunc(nargs = 2) takes place.

select dfunc();  -- fail
select dfunc(1);  -- fail
select dfunc(1, 2);  -- ok
select dfunc(1, 2, 3);  -- ok
select dfunc(1, 2, 3, 4);  -- ok

drop function dfunc(int, int);
drop function dfunc(int, int, int, int);

-- default values are not allowed for output parameters
create function dfunc(out int = 20) returns int as $$
  select 1; 
$$ language sql;

-- polymorphic parameter test
create function dfunc(anyelement = 'World'::text) returns text as $$
  select 'Hello, ' || $1::text;
$$ language sql;

select dfunc();
select dfunc(0);
select dfunc(to_date('20081215','YYYYMMDD'));
select dfunc('City'::text);

drop function dfunc(anyelement);

-- check null values
create function dfunc(int = null, int = null, int = null) returns int[] as $$
  select array[$1, $2, $3];
$$ language sql;

select dfunc(1);
select dfunc(1, 2);
select dfunc(1, 2, 3);

drop function dfunc(int, int, int);

-- The conflict detection algorithm doesn't consider the actual parameter
-- types.  It detects any possible conflict for n arguments for some
-- function.  This is unwanted behavior, but solving it needs a move of
-- coercion routines.

create function dfunc(int = 1, int = 2, int = 3) returns int as $$
  select 3;
$$ language sql;

create function dfunc(int = 1, int = 2) returns int as $$
  select 2;
$$ language sql;

-- for n = 1 dfunc(narg=2) and dfunc(narg=3) are ambiguous
select dfunc(1);  -- fail

create function dfunc(text) returns text as $$
  select $1;
$$ language sql;

-- Will fail, it detects ambiguity between dfunc(int, int, int) and
-- dfunc(int, int), but dfunc(text) isn't in conflict with either.
select dfunc('Hi');

drop function dfunc(int, int, int);
drop function dfunc(int, int);
drop function dfunc(text);
