--
-- OPR_SANITY
-- Sanity checks for common errors in making operator/procedure system tables:
-- pg_operator, pg_proc, pg_cast, pg_aggregate, pg_am, pg_amop, pg_amproc, pg_opclass.
--
-- None of the SELECTs here should ever find any matching entries,
-- so the expected output is easy to maintain ;-).
-- A test failure indicates someone messed up an entry in the system tables.
--
-- NB: we assume the oidjoins test will have caught any dangling links,
-- that is OID or REGPROC fields that are not zero and do not match some
-- row in the linked-to table.  However, if we want to enforce that a link
-- field can't be 0, we have to check it here.
--
-- NB: run this test earlier than the create_operator test, because
-- that test creates some bogus operators...


-- Helper functions to deal with cases where binary-coercible matches are
-- allowed.

-- This should match IsBinaryCoercible() in parse_coerce.c.
create function binary_coercible(oid, oid) returns bool as
'SELECT ($1 = $2) OR
 EXISTS(select 1 from pg_cast where
        castsource = $1 and casttarget = $2 and
        castfunc = 0 and castcontext = ''i'')'
language sql;

-- This one ignores castcontext, so it considers only physical equivalence
-- and not whether the coercion can be invoked implicitly.
create function physically_coercible(oid, oid) returns bool as
'SELECT ($1 = $2) OR
 EXISTS(select 1 from pg_cast where
        castsource = $1 and casttarget = $2 and
        castfunc = 0)'
language sql;

-- **************** pg_proc ****************

-- Look for illegal values in pg_proc fields.
-- NOTE: in reality pronargs could be more than 10, but I'm too lazy to put
-- a larger number of proargtypes check clauses in here.  If we ever have
-- more-than-10-arg functions in the standard catalogs, extend this query.

SELECT p1.oid, p1.proname
FROM pg_proc as p1
WHERE p1.prolang = 0 OR p1.prorettype = 0 OR
       p1.pronargs < 0 OR p1.pronargs > 10 OR
       (p1.proargtypes[0] = 0 AND p1.pronargs > 0) OR
       (p1.proargtypes[1] = 0 AND p1.pronargs > 1) OR
       (p1.proargtypes[2] = 0 AND p1.pronargs > 2) OR
       (p1.proargtypes[3] = 0 AND p1.pronargs > 3) OR
       (p1.proargtypes[4] = 0 AND p1.pronargs > 4) OR
       (p1.proargtypes[5] = 0 AND p1.pronargs > 5) OR
       (p1.proargtypes[6] = 0 AND p1.pronargs > 6) OR
       (p1.proargtypes[7] = 0 AND p1.pronargs > 7) OR
       (p1.proargtypes[8] = 0 AND p1.pronargs > 8) OR
       (p1.proargtypes[9] = 0 AND p1.pronargs > 9);

-- Look for conflicting proc definitions (same names and input datatypes).
-- (This test should be dead code now that we have the unique index
-- pg_proc_proname_narg_type_index, but I'll leave it in anyway.)

SELECT p1.oid, p1.proname, p2.oid, p2.proname
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.proname = p2.proname AND
    p1.pronargs = p2.pronargs AND
    p1.proargtypes = p2.proargtypes;

-- Considering only built-in procs (prolang = 12), look for multiple uses
-- of the same internal function (ie, matching prosrc fields).  It's OK to
-- have several entries with different pronames for the same internal function,
-- but conflicts in the number of arguments and other critical items should
-- be complained of.

SELECT p1.oid, p1.proname, p2.oid, p2.proname
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    p1.prolang = 12 AND p2.prolang = 12 AND
    (p1.prolang != p2.prolang OR
     p1.proisagg != p2.proisagg OR
     p1.prosecdef != p2.prosecdef OR
     p1.proisstrict != p2.proisstrict OR
     p1.proretset != p2.proretset OR
     p1.provolatile != p2.provolatile OR
     p1.pronargs != p2.pronargs);

-- Look for uses of different type OIDs in the argument/result type fields
-- for different aliases of the same built-in function.
-- This indicates that the types are being presumed to be binary-equivalent,
-- or that the built-in function is prepared to deal with different types.
-- That's not wrong, necessarily, but we make lists of all the types being
-- so treated.  Note that the expected output of this part of the test will
-- need to be modified whenever new pairs of types are made binary-equivalent,
-- or when new polymorphic built-in functions are added!
-- Note: ignore aggregate functions here, since they all point to the same
-- dummy built-in function.

SELECT DISTINCT p1.prorettype, p2.prorettype
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    p1.prolang = 12 AND p2.prolang = 12 AND
    NOT p1.proisagg AND NOT p2.proisagg AND
    (p1.prorettype < p2.prorettype);

SELECT DISTINCT p1.proargtypes[0], p2.proargtypes[0]
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    p1.prolang = 12 AND p2.prolang = 12 AND
    NOT p1.proisagg AND NOT p2.proisagg AND
    (p1.proargtypes[0] < p2.proargtypes[0]);

SELECT DISTINCT p1.proargtypes[1], p2.proargtypes[1]
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    p1.prolang = 12 AND p2.prolang = 12 AND
    NOT p1.proisagg AND NOT p2.proisagg AND
    (p1.proargtypes[1] < p2.proargtypes[1]);

SELECT DISTINCT p1.proargtypes[2], p2.proargtypes[2]
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    p1.prolang = 12 AND p2.prolang = 12 AND
    NOT p1.proisagg AND NOT p2.proisagg AND
    (p1.proargtypes[2] < p2.proargtypes[2]);

SELECT DISTINCT p1.proargtypes[3], p2.proargtypes[3]
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    p1.prolang = 12 AND p2.prolang = 12 AND
    NOT p1.proisagg AND NOT p2.proisagg AND
    (p1.proargtypes[3] < p2.proargtypes[3]);

SELECT DISTINCT p1.proargtypes[4], p2.proargtypes[4]
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    p1.prolang = 12 AND p2.prolang = 12 AND
    NOT p1.proisagg AND NOT p2.proisagg AND
    (p1.proargtypes[4] < p2.proargtypes[4]);

SELECT DISTINCT p1.proargtypes[5], p2.proargtypes[5]
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    p1.prolang = 12 AND p2.prolang = 12 AND
    NOT p1.proisagg AND NOT p2.proisagg AND
    (p1.proargtypes[5] < p2.proargtypes[5]);

SELECT DISTINCT p1.proargtypes[6], p2.proargtypes[6]
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    p1.prolang = 12 AND p2.prolang = 12 AND
    NOT p1.proisagg AND NOT p2.proisagg AND
    (p1.proargtypes[6] < p2.proargtypes[6]);

SELECT DISTINCT p1.proargtypes[7], p2.proargtypes[7]
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    p1.prolang = 12 AND p2.prolang = 12 AND
    NOT p1.proisagg AND NOT p2.proisagg AND
    (p1.proargtypes[7] < p2.proargtypes[7]);

-- Look for functions that return type "internal" and do not have any
-- "internal" argument.  Such a function would be a security hole since
-- it might be used to call an internal function from an SQL command.
-- As of 7.3 this query should find only internal_in.

SELECT p1.oid, p1.proname
FROM pg_proc as p1
WHERE p1.prorettype = 'internal'::regtype AND NOT
    ('(' || oidvectortypes(p1.proargtypes) || ')') ~ '[^a-z0-9_]internal[^a-z0-9_]';


-- **************** pg_cast ****************

-- Look for casts from and to the same type.  This is not harmful, but
-- useless.  Also catch bogus values in pg_cast columns (other than
-- cases detected by oidjoins test).

SELECT *
FROM pg_cast c
WHERE castsource = casttarget OR castsource = 0 OR casttarget = 0
    OR castcontext NOT IN ('e', 'a', 'i');

-- Look for cast functions that don't have the right signature.  The
-- argument and result types in pg_proc must be the same as, or binary
-- compatible with, what it says in pg_cast.

SELECT c.*
FROM pg_cast c, pg_proc p
WHERE c.castfunc = p.oid AND
    (p.pronargs <> 1
     OR NOT binary_coercible(c.castsource, p.proargtypes[0])
     OR NOT binary_coercible(p.prorettype, c.casttarget));

-- Look for binary compatible casts that do not have the reverse
-- direction registered as well, or where the reverse direction is not
-- also binary compatible.  This is legal, but usually not intended.

-- As of 7.4, this finds the casts from text and varchar to bpchar, because
-- those are binary-compatible while the reverse way goes through rtrim().

SELECT *
FROM pg_cast c
WHERE c.castfunc = 0 AND
    NOT EXISTS (SELECT 1 FROM pg_cast k
                WHERE k.castfunc = 0 AND
                    k.castsource = c.casttarget AND
                    k.casttarget = c.castsource);

-- **************** pg_operator ****************

-- Look for illegal values in pg_operator fields.

SELECT p1.oid, p1.oprname
FROM pg_operator as p1
WHERE (p1.oprkind != 'b' AND p1.oprkind != 'l' AND p1.oprkind != 'r') OR
    p1.oprresult = 0 OR p1.oprcode = 0;

-- Look for missing or unwanted operand types

SELECT p1.oid, p1.oprname
FROM pg_operator as p1
WHERE (p1.oprleft = 0 and p1.oprkind != 'l') OR
    (p1.oprleft != 0 and p1.oprkind = 'l') OR
    (p1.oprright = 0 and p1.oprkind != 'r') OR
    (p1.oprright != 0 and p1.oprkind = 'r');

-- Look for conflicting operator definitions (same names and input datatypes).

SELECT p1.oid, p1.oprcode, p2.oid, p2.oprcode
FROM pg_operator AS p1, pg_operator AS p2
WHERE p1.oid != p2.oid AND
    p1.oprname = p2.oprname AND
    p1.oprkind = p2.oprkind AND
    p1.oprleft = p2.oprleft AND
    p1.oprright = p2.oprright;

-- Look for commutative operators that don't commute.
-- DEFINITIONAL NOTE: If A.oprcom = B, then x A y has the same result as y B x.
-- We expect that B will always say that B.oprcom = A as well; that's not
-- inherently essential, but it would be inefficient not to mark it so.

SELECT p1.oid, p1.oprcode, p2.oid, p2.oprcode
FROM pg_operator AS p1, pg_operator AS p2
WHERE p1.oprcom = p2.oid AND
    (p1.oprkind != 'b' OR
     p1.oprleft != p2.oprright OR
     p1.oprright != p2.oprleft OR
     p1.oprresult != p2.oprresult OR
     p1.oid != p2.oprcom);

-- Look for negatory operators that don't agree.
-- DEFINITIONAL NOTE: If A.oprnegate = B, then both A and B must yield
-- boolean results, and (x A y) == ! (x B y), or the equivalent for
-- single-operand operators.
-- We expect that B will always say that B.oprnegate = A as well; that's not
-- inherently essential, but it would be inefficient not to mark it so.
-- Also, A and B had better not be the same operator.

SELECT p1.oid, p1.oprcode, p2.oid, p2.oprcode
FROM pg_operator AS p1, pg_operator AS p2
WHERE p1.oprnegate = p2.oid AND
    (p1.oprkind != p2.oprkind OR
     p1.oprleft != p2.oprleft OR
     p1.oprright != p2.oprright OR
     p1.oprresult != 'bool'::regtype OR
     p2.oprresult != 'bool'::regtype OR
     p1.oid != p2.oprnegate OR
     p1.oid = p2.oid);

-- Look for mergejoin operators that don't match their links.
-- An lsortop/rsortop link leads from an '=' operator to the
-- sort operator ('<' operator) that's appropriate for
-- its left-side or right-side data type.
-- An ltcmpop/gtcmpop link leads from an '=' operator to the
-- '<' or '>' operator of the same input datatypes.
-- (If the '=' operator has identical L and R input datatypes,
-- then lsortop, rsortop, and ltcmpop are all the same operator.)

SELECT p1.oid, p1.oprcode, p2.oid, p2.oprcode
FROM pg_operator AS p1, pg_operator AS p2
WHERE p1.oprlsortop = p2.oid AND
    (p1.oprname NOT IN ('=', '~=~') OR p2.oprname NOT IN ('<', '~<~') OR
     p1.oprkind != 'b' OR p2.oprkind != 'b' OR
     p1.oprleft != p2.oprleft OR
     p1.oprleft != p2.oprright OR
     p1.oprresult != 'bool'::regtype OR
     p2.oprresult != 'bool'::regtype);

SELECT p1.oid, p1.oprcode, p2.oid, p2.oprcode
FROM pg_operator AS p1, pg_operator AS p2
WHERE p1.oprrsortop = p2.oid AND
    (p1.oprname NOT IN ('=', '~=~') OR p2.oprname NOT IN ('<', '~<~') OR
     p1.oprkind != 'b' OR p2.oprkind != 'b' OR
     p1.oprright != p2.oprleft OR
     p1.oprright != p2.oprright OR
     p1.oprresult != 'bool'::regtype OR
     p2.oprresult != 'bool'::regtype);

SELECT p1.oid, p1.oprcode, p2.oid, p2.oprcode
FROM pg_operator AS p1, pg_operator AS p2
WHERE p1.oprltcmpop = p2.oid AND
    (p1.oprname NOT IN ('=', '~=~') OR p2.oprname NOT IN ('<', '~<~') OR
     p1.oprkind != 'b' OR p2.oprkind != 'b' OR
     p1.oprleft != p2.oprleft OR
     p1.oprright != p2.oprright OR
     p1.oprresult != 'bool'::regtype OR
     p2.oprresult != 'bool'::regtype);

SELECT p1.oid, p1.oprcode, p2.oid, p2.oprcode
FROM pg_operator AS p1, pg_operator AS p2
WHERE p1.oprgtcmpop = p2.oid AND
    (p1.oprname NOT IN ('=', '~=~') OR p2.oprname NOT IN ('>', '~>~') OR
     p1.oprkind != 'b' OR p2.oprkind != 'b' OR
     p1.oprleft != p2.oprleft OR
     p1.oprright != p2.oprright OR
     p1.oprresult != 'bool'::regtype OR
     p2.oprresult != 'bool'::regtype);

-- Make sure all four links are specified if any are.

SELECT p1.oid, p1.oprcode
FROM pg_operator AS p1
WHERE NOT ((oprlsortop = 0 AND oprrsortop = 0 AND
            oprltcmpop = 0 AND oprgtcmpop = 0) OR
           (oprlsortop != 0 AND oprrsortop != 0 AND
            oprltcmpop != 0 AND oprgtcmpop != 0));

-- A mergejoinable = operator must have a commutator (usually itself).

SELECT p1.oid, p1.oprname FROM pg_operator AS p1
WHERE p1.oprlsortop != 0 AND
      p1.oprcom = 0;

-- Mergejoinable operators across datatypes must come in closed sets, that
-- is if you provide int2 = int4 and int4 = int8 then you must also provide
-- int2 = int8 (and commutators of all these).  This is necessary because
-- the planner tries to deduce additional qual clauses from transitivity
-- of mergejoinable operators.  If there are clauses int2var = int4var and
-- int4var = int8var, the planner will deduce int2var = int8var ... and it
-- had better have a way to represent it.

SELECT p1.oid, p2.oid FROM pg_operator AS p1, pg_operator AS p2
WHERE p1.oprlsortop != p1.oprrsortop AND
      p1.oprrsortop = p2.oprlsortop AND
      p2.oprlsortop != p2.oprrsortop AND
      NOT EXISTS (SELECT 1 FROM pg_operator p3 WHERE
      p3.oprlsortop = p1.oprlsortop AND p3.oprrsortop = p2.oprrsortop);


-- Hashing only works on simple equality operators "type = sametype",
-- since the hash itself depends on the bitwise representation of the type.
-- Check that allegedly hashable operators look like they might be "=".

SELECT p1.oid, p1.oprname
FROM pg_operator AS p1
WHERE p1.oprcanhash AND NOT
    (p1.oprkind = 'b' AND p1.oprresult = 'bool'::regtype AND
     p1.oprleft = p1.oprright AND p1.oprname IN ('=', '~=~') AND
     p1.oprcom = p1.oid);

-- In 6.5 we accepted hashable array equality operators when the array element
-- type is hashable.  However, what we actually need to make hashjoin work on
-- an array is a hashable element type *and* no padding between elements in
-- the array storage (or, perhaps, guaranteed-zero padding).  Currently,
-- since the padding code in arrayfuncs.c is pretty bogus, it seems safest
-- to just forbid hashjoin on array equality ops.
-- This should be reconsidered someday.

-- -- Look for array equality operators that are hashable when the underlying
-- -- type is not, or vice versa.  This is presumably bogus.
-- 
-- SELECT p1.oid, p1.oprcanhash, p2.oid, p2.oprcanhash, t1.typname, t2.typname
-- FROM pg_operator AS p1, pg_operator AS p2, pg_type AS t1, pg_type AS t2
-- WHERE p1.oprname = '=' AND p1.oprleft = p1.oprright AND 
--     p2.oprname = '=' AND p2.oprleft = p2.oprright AND
--     p1.oprleft = t1.oid AND p2.oprleft = t2.oid AND t1.typelem = t2.oid AND
--     p1.oprcanhash != p2.oprcanhash;

-- Substitute check: forbid hashable array ops, period.
SELECT p1.oid, p1.oprname
FROM pg_operator AS p1, pg_proc AS p2
WHERE p1.oprcanhash AND p1.oprcode = p2.oid AND p2.proname = 'array_eq';

-- Hashable operators should appear as members of hash index opclasses.

SELECT p1.oid, p1.oprname
FROM pg_operator AS p1
WHERE p1.oprcanhash AND NOT EXISTS
  (SELECT 1 FROM pg_opclass op JOIN pg_amop p ON op.oid = amopclaid
   WHERE opcamid = (SELECT oid FROM pg_am WHERE amname = 'hash') AND
         amopopr = p1.oid);


-- Check that each operator defined in pg_operator matches its oprcode entry
-- in pg_proc.  Easiest to do this separately for each oprkind.

SELECT p1.oid, p1.oprname, p2.oid, p2.proname
FROM pg_operator AS p1, pg_proc AS p2
WHERE p1.oprcode = p2.oid AND
    p1.oprkind = 'b' AND
    (p2.pronargs != 2
     OR NOT binary_coercible(p2.prorettype, p1.oprresult)
     OR NOT binary_coercible(p1.oprleft, p2.proargtypes[0])
     OR NOT binary_coercible(p1.oprright, p2.proargtypes[1]));

SELECT p1.oid, p1.oprname, p2.oid, p2.proname
FROM pg_operator AS p1, pg_proc AS p2
WHERE p1.oprcode = p2.oid AND
    p1.oprkind = 'l' AND
    (p2.pronargs != 1
     OR NOT binary_coercible(p2.prorettype, p1.oprresult)
     OR NOT binary_coercible(p1.oprright, p2.proargtypes[0])
     OR p1.oprleft != 0);

SELECT p1.oid, p1.oprname, p2.oid, p2.proname
FROM pg_operator AS p1, pg_proc AS p2
WHERE p1.oprcode = p2.oid AND
    p1.oprkind = 'r' AND
    (p2.pronargs != 1
     OR NOT binary_coercible(p2.prorettype, p1.oprresult)
     OR NOT binary_coercible(p1.oprleft, p2.proargtypes[0])
     OR p1.oprright != 0);

-- If the operator is mergejoinable or hashjoinable, its underlying function
-- should not be volatile.

SELECT p1.oid, p1.oprname, p2.oid, p2.proname
FROM pg_operator AS p1, pg_proc AS p2
WHERE p1.oprcode = p2.oid AND
    (p1.oprlsortop != 0 OR p1.oprcanhash) AND
    p2.provolatile = 'v';

-- If oprrest is set, the operator must return boolean,
-- and it must link to a proc with the right signature
-- to be a restriction selectivity estimator.
-- The proc signature we want is: float8 proc(internal, oid, internal, int4)

SELECT p1.oid, p1.oprname, p2.oid, p2.proname
FROM pg_operator AS p1, pg_proc AS p2
WHERE p1.oprrest = p2.oid AND
    (p1.oprresult != 'bool'::regtype OR
     p2.prorettype != 'float8'::regtype OR p2.proretset OR
     p2.pronargs != 4 OR
     p2.proargtypes[0] != 'internal'::regtype OR
     p2.proargtypes[1] != 'oid'::regtype OR
     p2.proargtypes[2] != 'internal'::regtype OR
     p2.proargtypes[3] != 'int4'::regtype);

-- If oprjoin is set, the operator must be a binary boolean op,
-- and it must link to a proc with the right signature
-- to be a join selectivity estimator.
-- The proc signature we want is: float8 proc(internal, oid, internal, int2)

SELECT p1.oid, p1.oprname, p2.oid, p2.proname
FROM pg_operator AS p1, pg_proc AS p2
WHERE p1.oprjoin = p2.oid AND
    (p1.oprkind != 'b' OR p1.oprresult != 'bool'::regtype OR
     p2.prorettype != 'float8'::regtype OR p2.proretset OR
     p2.pronargs != 4 OR
     p2.proargtypes[0] != 'internal'::regtype OR
     p2.proargtypes[1] != 'oid'::regtype OR
     p2.proargtypes[2] != 'internal'::regtype OR
     p2.proargtypes[3] != 'int2'::regtype);

-- **************** pg_aggregate ****************

-- Look for illegal values in pg_aggregate fields.

SELECT ctid, aggfnoid::oid
FROM pg_aggregate as p1
WHERE aggfnoid = 0 OR aggtransfn = 0 OR aggtranstype = 0;

-- Make sure the matching pg_proc entry is sensible, too.

SELECT a.aggfnoid::oid, p.proname
FROM pg_aggregate as a, pg_proc as p
WHERE a.aggfnoid = p.oid AND
    (NOT p.proisagg OR p.pronargs != 1 OR p.proretset);

-- Make sure there are no proisagg pg_proc entries without matches.

SELECT oid, proname
FROM pg_proc as p
WHERE p.proisagg AND
    NOT EXISTS (SELECT 1 FROM pg_aggregate a WHERE a.aggfnoid = p.oid);

-- If there is no finalfn then the output type must be the transtype.

SELECT a.aggfnoid::oid, p.proname
FROM pg_aggregate as a, pg_proc as p
WHERE a.aggfnoid = p.oid AND
    a.aggfinalfn = 0 AND p.prorettype != a.aggtranstype;

-- Cross-check transfn against its entry in pg_proc.
-- NOTE: use physically_coercible here, not binary_coercible, because
-- max and min on abstime are implemented using int4larger/int4smaller.
SELECT a.aggfnoid::oid, p.proname, ptr.oid, ptr.proname
FROM pg_aggregate AS a, pg_proc AS p, pg_proc AS ptr
WHERE a.aggfnoid = p.oid AND
    a.aggtransfn = ptr.oid AND
    (ptr.proretset
     OR NOT physically_coercible(ptr.prorettype, a.aggtranstype)
     OR NOT physically_coercible(a.aggtranstype, ptr.proargtypes[0])
     OR NOT ((ptr.pronargs = 2 AND
              physically_coercible(p.proargtypes[0], ptr.proargtypes[1]))
             OR
             (ptr.pronargs = 1 AND
              p.proargtypes[0] = '"any"'::regtype)));

-- Cross-check finalfn (if present) against its entry in pg_proc.

SELECT a.aggfnoid::oid, p.proname, pfn.oid, pfn.proname
FROM pg_aggregate AS a, pg_proc AS p, pg_proc AS pfn
WHERE a.aggfnoid = p.oid AND
    a.aggfinalfn = pfn.oid AND
    (pfn.proretset
     OR NOT binary_coercible(pfn.prorettype, p.prorettype)
     OR pfn.pronargs != 1
     OR NOT binary_coercible(a.aggtranstype, pfn.proargtypes[0]));

-- If transfn is strict then either initval should be non-NULL, or
-- input type should match transtype so that the first non-null input
-- can be assigned as the state value.

SELECT a.aggfnoid::oid, p.proname, ptr.oid, ptr.proname
FROM pg_aggregate AS a, pg_proc AS p, pg_proc AS ptr
WHERE a.aggfnoid = p.oid AND
    a.aggtransfn = ptr.oid AND ptr.proisstrict AND
    a.agginitval IS NULL AND
    NOT binary_coercible(p.proargtypes[0], a.aggtranstype);

-- **************** pg_opclass ****************

-- Look for illegal values in pg_opclass fields

SELECT p1.oid
FROM pg_opclass as p1
WHERE p1.opcamid = 0 OR p1.opcintype = 0;

-- There should not be multiple entries in pg_opclass with opcdefault true
-- and the same opcamid/opcintype combination.

SELECT p1.oid, p2.oid
FROM pg_opclass AS p1, pg_opclass AS p2
WHERE p1.oid != p2.oid AND
    p1.opcamid = p2.opcamid AND p1.opcintype = p2.opcintype AND
    p1.opcdefault AND p2.opcdefault;

-- **************** pg_amop ****************

-- Look for illegal values in pg_amop fields

SELECT p1.amopclaid, p1.amopstrategy
FROM pg_amop as p1
WHERE p1.amopclaid = 0 OR p1.amopstrategy <= 0 OR p1.amopopr = 0;

-- Cross-check amopstrategy index against parent AM

SELECT p1.amopclaid, p1.amopopr, p2.oid, p2.amname
FROM pg_amop AS p1, pg_am AS p2, pg_opclass AS p3
WHERE p1.amopclaid = p3.oid AND p3.opcamid = p2.oid AND
    p1.amopstrategy > p2.amstrategies;

-- Detect missing pg_amop entries: should have as many strategy functions
-- as AM expects for each opclass for the AM

SELECT p1.oid, p1.amname, p2.oid, p2.opcname
FROM pg_am AS p1, pg_opclass AS p2
WHERE p2.opcamid = p1.oid AND
    p1.amstrategies != (SELECT count(*) FROM pg_amop AS p3
                        WHERE p3.amopclaid = p2.oid);

-- Check that amopopr points at a reasonable-looking operator, ie a binary
-- operator yielding boolean.
-- NOTE: for 7.1, add restriction that operator inputs are of same type.
-- We used to have opclasses like "int24_ops" but these were broken.

SELECT p1.amopclaid, p1.amopopr, p2.oid, p2.oprname
FROM pg_amop AS p1, pg_operator AS p2
WHERE p1.amopopr = p2.oid AND
    (p2.oprkind != 'b' OR p2.oprresult != 'bool'::regtype OR
     p2.oprleft != p2.oprright);

-- Check that all operators linked to by opclass entries have selectivity
-- estimators.  This is not absolutely required, but it seems a reasonable
-- thing to insist on for all standard datatypes.

SELECT p1.amopclaid, p1.amopopr, p2.oid, p2.oprname
FROM pg_amop AS p1, pg_operator AS p2
WHERE p1.amopopr = p2.oid AND
    (p2.oprrest = 0 OR p2.oprjoin = 0);

-- Check that operator input types match the opclass

SELECT p1.amopclaid, p1.amopopr, p2.oid, p2.oprname, p3.opcname
FROM pg_amop AS p1, pg_operator AS p2, pg_opclass AS p3
WHERE p1.amopopr = p2.oid AND p1.amopclaid = p3.oid AND
    (NOT binary_coercible(p3.opcintype, p2.oprleft) OR
     p2.oprleft != p2.oprright);

-- **************** pg_amproc ****************

-- Look for illegal values in pg_amproc fields

SELECT p1.amopclaid, p1.amprocnum
FROM pg_amproc as p1
WHERE p1.amopclaid = 0 OR p1.amprocnum <= 0 OR p1.amproc = 0;

-- Cross-check amprocnum index against parent AM

SELECT p1.amopclaid, p1.amprocnum, p2.oid, p2.amname
FROM pg_amproc AS p1, pg_am AS p2, pg_opclass AS p3
WHERE p1.amopclaid = p3.oid AND p3.opcamid = p2.oid AND
    p1.amprocnum > p2.amsupport;

-- Detect missing pg_amproc entries: should have as many support functions
-- as AM expects for each opclass for the AM

SELECT p1.oid, p1.amname, p2.oid, p2.opcname
FROM pg_am AS p1, pg_opclass AS p2
WHERE p2.opcamid = p1.oid AND
    p1.amsupport != (SELECT count(*) FROM pg_amproc AS p3
                     WHERE p3.amopclaid = p2.oid);

-- Unfortunately, we can't check the amproc link very well because the
-- signature of the function may be different for different support routines
-- or different base data types.
-- We can check that all the referenced instances of the same support
-- routine number take the same number of parameters, but that's about it
-- for a general check...

SELECT p1.amopclaid, p1.amprocnum,
	p2.oid, p2.proname,
	p3.opcname,
	p4.amopclaid, p4.amprocnum,
	p5.oid, p5.proname,
	p6.opcname
FROM pg_amproc AS p1, pg_proc AS p2, pg_opclass AS p3,
     pg_amproc AS p4, pg_proc AS p5, pg_opclass AS p6
WHERE p1.amopclaid = p3.oid AND p4.amopclaid = p6.oid AND
    p3.opcamid = p6.opcamid AND p1.amprocnum = p4.amprocnum AND
    p1.amproc = p2.oid AND p4.amproc = p5.oid AND
    (p2.proretset OR p5.proretset OR p2.pronargs != p5.pronargs);

-- For btree, though, we can do better since we know the support routines
-- must be of the form cmp(input, input) returns int4.

SELECT p1.amopclaid, p1.amprocnum,
	p2.oid, p2.proname,
	p3.opcname
FROM pg_amproc AS p1, pg_proc AS p2, pg_opclass AS p3
WHERE p3.opcamid = (SELECT oid FROM pg_am WHERE amname = 'btree')
    AND p1.amopclaid = p3.oid AND p1.amproc = p2.oid AND
    (opckeytype != 0
     OR amprocnum != 1
     OR proretset
     OR prorettype != 23
     OR pronargs != 2
     OR NOT binary_coercible(opcintype, proargtypes[0])
     OR proargtypes[0] != proargtypes[1]);

-- For hash we can also do a little better: the support routines must be
-- of the form hash(something) returns int4.  Ideally we'd check that the
-- opcintype is binary-coercible to the function's input, but there are
-- enough cases where that fails that I'll just leave out the check for now.

SELECT p1.amopclaid, p1.amprocnum,
	p2.oid, p2.proname,
	p3.opcname
FROM pg_amproc AS p1, pg_proc AS p2, pg_opclass AS p3
WHERE p3.opcamid = (SELECT oid FROM pg_am WHERE amname = 'hash')
    AND p1.amopclaid = p3.oid AND p1.amproc = p2.oid AND
    (opckeytype != 0
     OR amprocnum != 1
     OR proretset
     OR prorettype != 23
     OR pronargs != 1
--   OR NOT physically_coercible(opcintype, proargtypes[0])
);
