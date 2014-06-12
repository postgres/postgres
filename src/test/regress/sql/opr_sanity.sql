--
-- OPR_SANITY
-- Sanity checks for common errors in making operator/procedure system tables:
-- pg_operator, pg_proc, pg_cast, pg_aggregate, pg_am,
-- pg_amop, pg_amproc, pg_opclass, pg_opfamily.
--
-- Every test failures in this file should be closely inspected. The
-- description of the failing test should be read carefully before
-- adjusting the expected output.
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
create function binary_coercible(oid, oid) returns bool as $$
SELECT ($1 = $2) OR
 EXISTS(select 1 from pg_catalog.pg_cast where
        castsource = $1 and casttarget = $2 and
        castmethod = 'b' and castcontext = 'i') OR
 ($2 = 'pg_catalog.any'::pg_catalog.regtype) OR
 ($2 = 'pg_catalog.anyarray'::pg_catalog.regtype AND
  EXISTS(select 1 from pg_catalog.pg_type where
         oid = $1 and typelem != 0 and typlen = -1))
$$ language sql strict stable;

-- This one ignores castcontext, so it considers only physical equivalence
-- and not whether the coercion can be invoked implicitly.
create function physically_coercible(oid, oid) returns bool as $$
SELECT ($1 = $2) OR
 EXISTS(select 1 from pg_catalog.pg_cast where
        castsource = $1 and casttarget = $2 and
        castmethod = 'b') OR
 ($2 = 'pg_catalog.any'::pg_catalog.regtype) OR
 ($2 = 'pg_catalog.anyarray'::pg_catalog.regtype AND
  EXISTS(select 1 from pg_catalog.pg_type where
         oid = $1 and typelem != 0 and typlen = -1))
$$ language sql strict stable;

-- **************** pg_proc ****************

-- Look for illegal values in pg_proc fields.

SELECT p1.oid, p1.proname
FROM pg_proc as p1
WHERE p1.prolang = 0 OR p1.prorettype = 0 OR
       p1.pronargs < 0 OR
       p1.pronargdefaults < 0 OR
       p1.pronargdefaults > p1.pronargs OR
       array_lower(p1.proargtypes, 1) != 0 OR
       array_upper(p1.proargtypes, 1) != p1.pronargs-1 OR
       0::oid = ANY (p1.proargtypes) OR
       procost <= 0 OR
       CASE WHEN proretset THEN prorows <= 0 ELSE prorows != 0 END;

-- prosrc should never be null or empty
SELECT p1.oid, p1.proname
FROM pg_proc as p1
WHERE prosrc IS NULL OR prosrc = '' OR prosrc = '-';

-- proiswindow shouldn't be set together with proisagg or proretset
SELECT p1.oid, p1.proname
FROM pg_proc AS p1
WHERE proiswindow AND (proisagg OR proretset);

-- pronargdefaults should be 0 iff proargdefaults is null
SELECT p1.oid, p1.proname
FROM pg_proc AS p1
WHERE (pronargdefaults <> 0) != (proargdefaults IS NOT NULL);

-- probin should be non-empty for C functions, null everywhere else
SELECT p1.oid, p1.proname
FROM pg_proc as p1
WHERE prolang = 13 AND (probin IS NULL OR probin = '' OR probin = '-');

SELECT p1.oid, p1.proname
FROM pg_proc as p1
WHERE prolang != 13 AND probin IS NOT NULL;

-- Look for conflicting proc definitions (same names and input datatypes).
-- (This test should be dead code now that we have the unique index
-- pg_proc_proname_args_nsp_index, but I'll leave it in anyway.)

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
-- be complained of.  (We don't check data types here; see next query.)
-- Note: ignore aggregate functions here, since they all point to the same
-- dummy built-in function.

SELECT p1.oid, p1.proname, p2.oid, p2.proname
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid < p2.oid AND
    p1.prosrc = p2.prosrc AND
    p1.prolang = 12 AND p2.prolang = 12 AND
    (p1.proisagg = false OR p2.proisagg = false) AND
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
-- dummy built-in function.  Likewise, ignore range constructor functions.

SELECT DISTINCT p1.prorettype, p2.prorettype
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    p1.prolang = 12 AND p2.prolang = 12 AND
    NOT p1.proisagg AND NOT p2.proisagg AND
    p1.prosrc NOT LIKE E'range\\_constructor_' AND
    p2.prosrc NOT LIKE E'range\\_constructor_' AND
    (p1.prorettype < p2.prorettype)
ORDER BY 1, 2;

SELECT DISTINCT p1.proargtypes[0], p2.proargtypes[0]
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    p1.prolang = 12 AND p2.prolang = 12 AND
    NOT p1.proisagg AND NOT p2.proisagg AND
    p1.prosrc NOT LIKE E'range\\_constructor_' AND
    p2.prosrc NOT LIKE E'range\\_constructor_' AND
    (p1.proargtypes[0] < p2.proargtypes[0])
ORDER BY 1, 2;

SELECT DISTINCT p1.proargtypes[1], p2.proargtypes[1]
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    p1.prolang = 12 AND p2.prolang = 12 AND
    NOT p1.proisagg AND NOT p2.proisagg AND
    p1.prosrc NOT LIKE E'range\\_constructor_' AND
    p2.prosrc NOT LIKE E'range\\_constructor_' AND
    (p1.proargtypes[1] < p2.proargtypes[1])
ORDER BY 1, 2;

SELECT DISTINCT p1.proargtypes[2], p2.proargtypes[2]
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    p1.prolang = 12 AND p2.prolang = 12 AND
    NOT p1.proisagg AND NOT p2.proisagg AND
    (p1.proargtypes[2] < p2.proargtypes[2])
ORDER BY 1, 2;

SELECT DISTINCT p1.proargtypes[3], p2.proargtypes[3]
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    p1.prolang = 12 AND p2.prolang = 12 AND
    NOT p1.proisagg AND NOT p2.proisagg AND
    (p1.proargtypes[3] < p2.proargtypes[3])
ORDER BY 1, 2;

SELECT DISTINCT p1.proargtypes[4], p2.proargtypes[4]
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    p1.prolang = 12 AND p2.prolang = 12 AND
    NOT p1.proisagg AND NOT p2.proisagg AND
    (p1.proargtypes[4] < p2.proargtypes[4])
ORDER BY 1, 2;

SELECT DISTINCT p1.proargtypes[5], p2.proargtypes[5]
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    p1.prolang = 12 AND p2.prolang = 12 AND
    NOT p1.proisagg AND NOT p2.proisagg AND
    (p1.proargtypes[5] < p2.proargtypes[5])
ORDER BY 1, 2;

SELECT DISTINCT p1.proargtypes[6], p2.proargtypes[6]
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    p1.prolang = 12 AND p2.prolang = 12 AND
    NOT p1.proisagg AND NOT p2.proisagg AND
    (p1.proargtypes[6] < p2.proargtypes[6])
ORDER BY 1, 2;

SELECT DISTINCT p1.proargtypes[7], p2.proargtypes[7]
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    p1.prolang = 12 AND p2.prolang = 12 AND
    NOT p1.proisagg AND NOT p2.proisagg AND
    (p1.proargtypes[7] < p2.proargtypes[7])
ORDER BY 1, 2;

-- Look for functions that return type "internal" and do not have any
-- "internal" argument.  Such a function would be a security hole since
-- it might be used to call an internal function from an SQL command.
-- As of 7.3 this query should find only internal_in.

SELECT p1.oid, p1.proname
FROM pg_proc as p1
WHERE p1.prorettype = 'internal'::regtype AND NOT
    'internal'::regtype = ANY (p1.proargtypes);

-- Look for functions that return a polymorphic type and do not have any
-- polymorphic argument.  Calls of such functions would be unresolvable
-- at parse time.  As of 9.4 this query should find only some input functions
-- associated with these pseudotypes.

SELECT p1.oid, p1.proname
FROM pg_proc as p1
WHERE p1.prorettype IN
    ('anyelement'::regtype, 'anyarray'::regtype, 'anynonarray'::regtype,
     'anyenum'::regtype, 'anyrange'::regtype)
  AND NOT
    ('anyelement'::regtype = ANY (p1.proargtypes) OR
     'anyarray'::regtype = ANY (p1.proargtypes) OR
     'anynonarray'::regtype = ANY (p1.proargtypes) OR
     'anyenum'::regtype = ANY (p1.proargtypes) OR
     'anyrange'::regtype = ANY (p1.proargtypes))
ORDER BY 2;

-- Check for length inconsistencies between the various argument-info arrays.

SELECT p1.oid, p1.proname
FROM pg_proc as p1
WHERE proallargtypes IS NOT NULL AND
    array_length(proallargtypes,1) < array_length(proargtypes,1);

SELECT p1.oid, p1.proname
FROM pg_proc as p1
WHERE proargmodes IS NOT NULL AND
    array_length(proargmodes,1) < array_length(proargtypes,1);

SELECT p1.oid, p1.proname
FROM pg_proc as p1
WHERE proargnames IS NOT NULL AND
    array_length(proargnames,1) < array_length(proargtypes,1);

SELECT p1.oid, p1.proname
FROM pg_proc as p1
WHERE proallargtypes IS NOT NULL AND proargmodes IS NOT NULL AND
    array_length(proallargtypes,1) <> array_length(proargmodes,1);

SELECT p1.oid, p1.proname
FROM pg_proc as p1
WHERE proallargtypes IS NOT NULL AND proargnames IS NOT NULL AND
    array_length(proallargtypes,1) <> array_length(proargnames,1);

SELECT p1.oid, p1.proname
FROM pg_proc as p1
WHERE proargmodes IS NOT NULL AND proargnames IS NOT NULL AND
    array_length(proargmodes,1) <> array_length(proargnames,1);

-- Check that proallargtypes matches proargtypes
SELECT p1.oid, p1.proname, p1.proargtypes, p1.proallargtypes, p1.proargmodes
FROM pg_proc as p1
WHERE proallargtypes IS NOT NULL AND
  ARRAY(SELECT unnest(proargtypes)) <>
  ARRAY(SELECT proallargtypes[i]
        FROM generate_series(1, array_length(proallargtypes, 1)) g(i)
        WHERE proargmodes IS NULL OR proargmodes[i] IN ('i', 'b', 'v'));

-- Check for protransform functions with the wrong signature
SELECT p1.oid, p1.proname, p2.oid, p2.proname
FROM pg_proc AS p1, pg_proc AS p2
WHERE p2.oid = p1.protransform AND
    (p2.prorettype != 'internal'::regtype OR p2.proretset OR p2.pronargs != 1
     OR p2.proargtypes[0] != 'internal'::regtype);

-- Insist that all built-in pg_proc entries have descriptions
SELECT p1.oid, p1.proname
FROM pg_proc as p1 LEFT JOIN pg_description as d
     ON p1.tableoid = d.classoid and p1.oid = d.objoid and d.objsubid = 0
WHERE d.classoid IS NULL AND p1.oid <= 9999;

-- List of built-in leakproof functions
--
-- Leakproof functions should only be added after carefully
-- scrutinizing all possibly executed codepaths for possible
-- information leaks. Don't add functions here unless you know what a
-- leakproof function is. If unsure, don't mark it as such.

-- temporarily disable fancy output, so catalog changes create less diff noise
\a\t

SELECT p1.oid::regprocedure
FROM pg_proc p1 JOIN pg_namespace pn
     ON pronamespace = pn.oid
WHERE nspname = 'pg_catalog' AND proleakproof
ORDER BY 1;

-- restore normal output mode
\a\t

-- List of functions used by libpq's fe-lobj.c
--
-- If the output of this query changes, you probably broke libpq.
-- lo_initialize() assumes that there will be at most one match for
-- each listed name.
select proname, oid from pg_catalog.pg_proc
where proname in (
  'lo_open',
  'lo_close',
  'lo_creat',
  'lo_create',
  'lo_unlink',
  'lo_lseek',
  'lo_lseek64',
  'lo_tell',
  'lo_tell64',
  'lo_truncate',
  'lo_truncate64',
  'loread',
  'lowrite')
and pronamespace = (select oid from pg_catalog.pg_namespace
                    where nspname = 'pg_catalog')
order by 1;


-- **************** pg_cast ****************

-- Catch bogus values in pg_cast columns (other than cases detected by
-- oidjoins test).

SELECT *
FROM pg_cast c
WHERE castsource = 0 OR casttarget = 0 OR castcontext NOT IN ('e', 'a', 'i')
    OR castmethod NOT IN ('f', 'b' ,'i');

-- Check that castfunc is nonzero only for cast methods that need a function,
-- and zero otherwise

SELECT *
FROM pg_cast c
WHERE (castmethod = 'f' AND castfunc = 0)
   OR (castmethod IN ('b', 'i') AND castfunc <> 0);

-- Look for casts to/from the same type that aren't length coercion functions.
-- (We assume they are length coercions if they take multiple arguments.)
-- Such entries are not necessarily harmful, but they are useless.

SELECT *
FROM pg_cast c
WHERE castsource = casttarget AND castfunc = 0;

SELECT c.*
FROM pg_cast c, pg_proc p
WHERE c.castfunc = p.oid AND p.pronargs < 2 AND castsource = casttarget;

-- Look for cast functions that don't have the right signature.  The
-- argument and result types in pg_proc must be the same as, or binary
-- compatible with, what it says in pg_cast.
-- As a special case, we allow casts from CHAR(n) that use functions
-- declared to take TEXT.  This does not pass the binary-coercibility test
-- because CHAR(n)-to-TEXT normally invokes rtrim().  However, the results
-- are the same, so long as the function is one that ignores trailing blanks.

SELECT c.*
FROM pg_cast c, pg_proc p
WHERE c.castfunc = p.oid AND
    (p.pronargs < 1 OR p.pronargs > 3
     OR NOT (binary_coercible(c.castsource, p.proargtypes[0])
             OR (c.castsource = 'character'::regtype AND
                 p.proargtypes[0] = 'text'::regtype))
     OR NOT binary_coercible(p.prorettype, c.casttarget));

SELECT c.*
FROM pg_cast c, pg_proc p
WHERE c.castfunc = p.oid AND
    ((p.pronargs > 1 AND p.proargtypes[1] != 'int4'::regtype) OR
     (p.pronargs > 2 AND p.proargtypes[2] != 'bool'::regtype));

-- Look for binary compatible casts that do not have the reverse
-- direction registered as well, or where the reverse direction is not
-- also binary compatible.  This is legal, but usually not intended.

-- As of 7.4, this finds the casts from text and varchar to bpchar, because
-- those are binary-compatible while the reverse way goes through rtrim().

-- As of 8.2, this finds the cast from cidr to inet, because that is a
-- trivial binary coercion while the other way goes through inet_to_cidr().

-- As of 8.3, this finds the casts from xml to text, varchar, and bpchar,
-- because those are binary-compatible while the reverse goes through
-- texttoxml(), which does an XML syntax check.

-- As of 9.1, this finds the cast from pg_node_tree to text, which we
-- intentionally do not provide a reverse pathway for.

SELECT castsource::regtype, casttarget::regtype, castfunc, castcontext
FROM pg_cast c
WHERE c.castmethod = 'b' AND
    NOT EXISTS (SELECT 1 FROM pg_cast k
                WHERE k.castmethod = 'b' AND
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

-- A mergejoinable or hashjoinable operator must be binary, must return
-- boolean, and must have a commutator (itself, unless it's a cross-type
-- operator).

SELECT p1.oid, p1.oprname FROM pg_operator AS p1
WHERE (p1.oprcanmerge OR p1.oprcanhash) AND NOT
    (p1.oprkind = 'b' AND p1.oprresult = 'bool'::regtype AND p1.oprcom != 0);

-- What's more, the commutator had better be mergejoinable/hashjoinable too.

SELECT p1.oid, p1.oprname, p2.oid, p2.oprname
FROM pg_operator AS p1, pg_operator AS p2
WHERE p1.oprcom = p2.oid AND
    (p1.oprcanmerge != p2.oprcanmerge OR
     p1.oprcanhash != p2.oprcanhash);

-- Mergejoinable operators should appear as equality members of btree index
-- opfamilies.

SELECT p1.oid, p1.oprname
FROM pg_operator AS p1
WHERE p1.oprcanmerge AND NOT EXISTS
  (SELECT 1 FROM pg_amop
   WHERE amopmethod = (SELECT oid FROM pg_am WHERE amname = 'btree') AND
         amopopr = p1.oid AND amopstrategy = 3);

-- And the converse.

SELECT p1.oid, p1.oprname, p.amopfamily
FROM pg_operator AS p1, pg_amop p
WHERE amopopr = p1.oid
  AND amopmethod = (SELECT oid FROM pg_am WHERE amname = 'btree')
  AND amopstrategy = 3
  AND NOT p1.oprcanmerge;

-- Hashable operators should appear as members of hash index opfamilies.

SELECT p1.oid, p1.oprname
FROM pg_operator AS p1
WHERE p1.oprcanhash AND NOT EXISTS
  (SELECT 1 FROM pg_amop
   WHERE amopmethod = (SELECT oid FROM pg_am WHERE amname = 'hash') AND
         amopopr = p1.oid AND amopstrategy = 1);

-- And the converse.

SELECT p1.oid, p1.oprname, p.amopfamily
FROM pg_operator AS p1, pg_amop p
WHERE amopopr = p1.oid
  AND amopmethod = (SELECT oid FROM pg_am WHERE amname = 'hash')
  AND NOT p1.oprcanhash;

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
    (p1.oprcanmerge OR p1.oprcanhash) AND
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
-- The proc signature we want is: float8 proc(internal, oid, internal, int2, internal)
-- (Note: the old signature with only 4 args is still allowed, but no core
-- estimator should be using it.)

SELECT p1.oid, p1.oprname, p2.oid, p2.proname
FROM pg_operator AS p1, pg_proc AS p2
WHERE p1.oprjoin = p2.oid AND
    (p1.oprkind != 'b' OR p1.oprresult != 'bool'::regtype OR
     p2.prorettype != 'float8'::regtype OR p2.proretset OR
     p2.pronargs != 5 OR
     p2.proargtypes[0] != 'internal'::regtype OR
     p2.proargtypes[1] != 'oid'::regtype OR
     p2.proargtypes[2] != 'internal'::regtype OR
     p2.proargtypes[3] != 'int2'::regtype OR
     p2.proargtypes[4] != 'internal'::regtype);

-- Insist that all built-in pg_operator entries have descriptions
SELECT p1.oid, p1.oprname
FROM pg_operator as p1 LEFT JOIN pg_description as d
     ON p1.tableoid = d.classoid and p1.oid = d.objoid and d.objsubid = 0
WHERE d.classoid IS NULL AND p1.oid <= 9999;

-- Check that operators' underlying functions have suitable comments,
-- namely 'implementation of XXX operator'.  (Note: it's not necessary to
-- put such comments into pg_proc.h; initdb will generate them as needed.)
-- In some cases involving legacy names for operators, there are multiple
-- operators referencing the same pg_proc entry, so ignore operators whose
-- comments say they are deprecated.
-- We also have a few functions that are both operator support and meant to
-- be called directly; those should have comments matching their operator.
WITH funcdescs AS (
  SELECT p.oid as p_oid, proname, o.oid as o_oid,
    obj_description(p.oid, 'pg_proc') as prodesc,
    'implementation of ' || oprname || ' operator' as expecteddesc,
    obj_description(o.oid, 'pg_operator') as oprdesc
  FROM pg_proc p JOIN pg_operator o ON oprcode = p.oid
  WHERE o.oid <= 9999
)
SELECT * FROM funcdescs
  WHERE prodesc IS DISTINCT FROM expecteddesc
    AND oprdesc NOT LIKE 'deprecated%'
    AND prodesc IS DISTINCT FROM oprdesc;

-- Show all the operator-implementation functions that have their own
-- comments.  This should happen only in cases where the function and
-- operator syntaxes are both documented at the user level.
-- This should be a pretty short list; it's mostly legacy cases.
WITH funcdescs AS (
  SELECT p.oid as p_oid, proname, o.oid as o_oid,
    obj_description(p.oid, 'pg_proc') as prodesc,
    'implementation of ' || oprname || ' operator' as expecteddesc,
    obj_description(o.oid, 'pg_operator') as oprdesc
  FROM pg_proc p JOIN pg_operator o ON oprcode = p.oid
  WHERE o.oid <= 9999
)
SELECT p_oid, proname, prodesc FROM funcdescs
  WHERE prodesc IS DISTINCT FROM expecteddesc
    AND oprdesc NOT LIKE 'deprecated%'
ORDER BY 1;


-- **************** pg_aggregate ****************

-- Look for illegal values in pg_aggregate fields.

SELECT ctid, aggfnoid::oid
FROM pg_aggregate as p1
WHERE aggfnoid = 0 OR aggtransfn = 0 OR
    aggkind NOT IN ('n', 'o', 'h') OR
    aggnumdirectargs < 0 OR
    (aggkind = 'n' AND aggnumdirectargs > 0) OR
    aggtranstype = 0 OR aggtransspace < 0 OR aggmtransspace < 0;

-- Make sure the matching pg_proc entry is sensible, too.

SELECT a.aggfnoid::oid, p.proname
FROM pg_aggregate as a, pg_proc as p
WHERE a.aggfnoid = p.oid AND
    (NOT p.proisagg OR p.proretset OR p.pronargs < a.aggnumdirectargs);

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
     OR NOT (ptr.pronargs =
             CASE WHEN a.aggkind = 'n' THEN p.pronargs + 1
             ELSE greatest(p.pronargs - a.aggnumdirectargs, 1) + 1 END)
     OR NOT physically_coercible(ptr.prorettype, a.aggtranstype)
     OR NOT physically_coercible(a.aggtranstype, ptr.proargtypes[0])
     OR (p.pronargs > 0 AND
         NOT physically_coercible(p.proargtypes[0], ptr.proargtypes[1]))
     OR (p.pronargs > 1 AND
         NOT physically_coercible(p.proargtypes[1], ptr.proargtypes[2]))
     OR (p.pronargs > 2 AND
         NOT physically_coercible(p.proargtypes[2], ptr.proargtypes[3]))
     -- we could carry the check further, but 3 args is enough for now
    );

-- Cross-check finalfn (if present) against its entry in pg_proc.

SELECT a.aggfnoid::oid, p.proname, pfn.oid, pfn.proname
FROM pg_aggregate AS a, pg_proc AS p, pg_proc AS pfn
WHERE a.aggfnoid = p.oid AND
    a.aggfinalfn = pfn.oid AND
    (pfn.proretset OR
     NOT binary_coercible(pfn.prorettype, p.prorettype) OR
     NOT binary_coercible(a.aggtranstype, pfn.proargtypes[0]) OR
     CASE WHEN a.aggfinalextra THEN pfn.pronargs != p.pronargs + 1
          ELSE pfn.pronargs != a.aggnumdirectargs + 1 END
     OR (pfn.pronargs > 1 AND
         NOT binary_coercible(p.proargtypes[0], pfn.proargtypes[1]))
     OR (pfn.pronargs > 2 AND
         NOT binary_coercible(p.proargtypes[1], pfn.proargtypes[2]))
     OR (pfn.pronargs > 3 AND
         NOT binary_coercible(p.proargtypes[2], pfn.proargtypes[3]))
     -- we could carry the check further, but 3 args is enough for now
    );

-- If transfn is strict then either initval should be non-NULL, or
-- input type should match transtype so that the first non-null input
-- can be assigned as the state value.

SELECT a.aggfnoid::oid, p.proname, ptr.oid, ptr.proname
FROM pg_aggregate AS a, pg_proc AS p, pg_proc AS ptr
WHERE a.aggfnoid = p.oid AND
    a.aggtransfn = ptr.oid AND ptr.proisstrict AND
    a.agginitval IS NULL AND
    NOT binary_coercible(p.proargtypes[0], a.aggtranstype);

-- Check for inconsistent specifications of moving-aggregate columns.

SELECT ctid, aggfnoid::oid
FROM pg_aggregate as p1
WHERE aggmtranstype != 0 AND
    (aggmtransfn = 0 OR aggminvtransfn = 0);

SELECT ctid, aggfnoid::oid
FROM pg_aggregate as p1
WHERE aggmtranstype = 0 AND
    (aggmtransfn != 0 OR aggminvtransfn != 0 OR aggmfinalfn != 0 OR
     aggmtransspace != 0 OR aggminitval IS NOT NULL);

-- If there is no mfinalfn then the output type must be the mtranstype.

SELECT a.aggfnoid::oid, p.proname
FROM pg_aggregate as a, pg_proc as p
WHERE a.aggfnoid = p.oid AND
    a.aggmtransfn != 0 AND
    a.aggmfinalfn = 0 AND p.prorettype != a.aggmtranstype;

-- Cross-check mtransfn (if present) against its entry in pg_proc.
SELECT a.aggfnoid::oid, p.proname, ptr.oid, ptr.proname
FROM pg_aggregate AS a, pg_proc AS p, pg_proc AS ptr
WHERE a.aggfnoid = p.oid AND
    a.aggmtransfn = ptr.oid AND
    (ptr.proretset
     OR NOT (ptr.pronargs =
             CASE WHEN a.aggkind = 'n' THEN p.pronargs + 1
             ELSE greatest(p.pronargs - a.aggnumdirectargs, 1) + 1 END)
     OR NOT physically_coercible(ptr.prorettype, a.aggmtranstype)
     OR NOT physically_coercible(a.aggmtranstype, ptr.proargtypes[0])
     OR (p.pronargs > 0 AND
         NOT physically_coercible(p.proargtypes[0], ptr.proargtypes[1]))
     OR (p.pronargs > 1 AND
         NOT physically_coercible(p.proargtypes[1], ptr.proargtypes[2]))
     OR (p.pronargs > 2 AND
         NOT physically_coercible(p.proargtypes[2], ptr.proargtypes[3]))
     -- we could carry the check further, but 3 args is enough for now
    );

-- Cross-check minvtransfn (if present) against its entry in pg_proc.
SELECT a.aggfnoid::oid, p.proname, ptr.oid, ptr.proname
FROM pg_aggregate AS a, pg_proc AS p, pg_proc AS ptr
WHERE a.aggfnoid = p.oid AND
    a.aggminvtransfn = ptr.oid AND
    (ptr.proretset
     OR NOT (ptr.pronargs =
             CASE WHEN a.aggkind = 'n' THEN p.pronargs + 1
             ELSE greatest(p.pronargs - a.aggnumdirectargs, 1) + 1 END)
     OR NOT physically_coercible(ptr.prorettype, a.aggmtranstype)
     OR NOT physically_coercible(a.aggmtranstype, ptr.proargtypes[0])
     OR (p.pronargs > 0 AND
         NOT physically_coercible(p.proargtypes[0], ptr.proargtypes[1]))
     OR (p.pronargs > 1 AND
         NOT physically_coercible(p.proargtypes[1], ptr.proargtypes[2]))
     OR (p.pronargs > 2 AND
         NOT physically_coercible(p.proargtypes[2], ptr.proargtypes[3]))
     -- we could carry the check further, but 3 args is enough for now
    );

-- Cross-check mfinalfn (if present) against its entry in pg_proc.

SELECT a.aggfnoid::oid, p.proname, pfn.oid, pfn.proname
FROM pg_aggregate AS a, pg_proc AS p, pg_proc AS pfn
WHERE a.aggfnoid = p.oid AND
    a.aggmfinalfn = pfn.oid AND
    (pfn.proretset OR
     NOT binary_coercible(pfn.prorettype, p.prorettype) OR
     NOT binary_coercible(a.aggmtranstype, pfn.proargtypes[0]) OR
     CASE WHEN a.aggmfinalextra THEN pfn.pronargs != p.pronargs + 1
          ELSE pfn.pronargs != a.aggnumdirectargs + 1 END
     OR (pfn.pronargs > 1 AND
         NOT binary_coercible(p.proargtypes[0], pfn.proargtypes[1]))
     OR (pfn.pronargs > 2 AND
         NOT binary_coercible(p.proargtypes[1], pfn.proargtypes[2]))
     OR (pfn.pronargs > 3 AND
         NOT binary_coercible(p.proargtypes[2], pfn.proargtypes[3]))
     -- we could carry the check further, but 3 args is enough for now
    );

-- If mtransfn is strict then either minitval should be non-NULL, or
-- input type should match mtranstype so that the first non-null input
-- can be assigned as the state value.

SELECT a.aggfnoid::oid, p.proname, ptr.oid, ptr.proname
FROM pg_aggregate AS a, pg_proc AS p, pg_proc AS ptr
WHERE a.aggfnoid = p.oid AND
    a.aggmtransfn = ptr.oid AND ptr.proisstrict AND
    a.aggminitval IS NULL AND
    NOT binary_coercible(p.proargtypes[0], a.aggmtranstype);

-- mtransfn and minvtransfn should have same strictness setting.

SELECT a.aggfnoid::oid, p.proname, ptr.oid, ptr.proname, iptr.oid, iptr.proname
FROM pg_aggregate AS a, pg_proc AS p, pg_proc AS ptr, pg_proc AS iptr
WHERE a.aggfnoid = p.oid AND
    a.aggmtransfn = ptr.oid AND
    a.aggminvtransfn = iptr.oid AND
    ptr.proisstrict != iptr.proisstrict;

-- Cross-check aggsortop (if present) against pg_operator.
-- We expect to find entries for bool_and, bool_or, every, max, and min.

SELECT DISTINCT proname, oprname
FROM pg_operator AS o, pg_aggregate AS a, pg_proc AS p
WHERE a.aggfnoid = p.oid AND a.aggsortop = o.oid
ORDER BY 1, 2;

-- Check datatypes match

SELECT a.aggfnoid::oid, o.oid
FROM pg_operator AS o, pg_aggregate AS a, pg_proc AS p
WHERE a.aggfnoid = p.oid AND a.aggsortop = o.oid AND
    (oprkind != 'b' OR oprresult != 'boolean'::regtype
     OR oprleft != p.proargtypes[0] OR oprright != p.proargtypes[0]);

-- Check operator is a suitable btree opfamily member

SELECT a.aggfnoid::oid, o.oid
FROM pg_operator AS o, pg_aggregate AS a, pg_proc AS p
WHERE a.aggfnoid = p.oid AND a.aggsortop = o.oid AND
    NOT EXISTS(SELECT 1 FROM pg_amop
               WHERE amopmethod = (SELECT oid FROM pg_am WHERE amname = 'btree')
                     AND amopopr = o.oid
                     AND amoplefttype = o.oprleft
                     AND amoprighttype = o.oprright);

-- Check correspondence of btree strategies and names

SELECT DISTINCT proname, oprname, amopstrategy
FROM pg_operator AS o, pg_aggregate AS a, pg_proc AS p,
     pg_amop as ao
WHERE a.aggfnoid = p.oid AND a.aggsortop = o.oid AND
    amopopr = o.oid AND
    amopmethod = (SELECT oid FROM pg_am WHERE amname = 'btree')
ORDER BY 1, 2;

-- Check that there are not aggregates with the same name and different
-- numbers of arguments.  While not technically wrong, we have a project policy
-- to avoid this because it opens the door for confusion in connection with
-- ORDER BY: novices frequently put the ORDER BY in the wrong place.
-- See the fate of the single-argument form of string_agg() for history.
-- (Note: we don't forbid users from creating such aggregates; the policy is
-- just to think twice before creating built-in aggregates like this.)
-- The only aggregates that should show up here are count(x) and count(*).

SELECT p1.oid::regprocedure, p2.oid::regprocedure
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid < p2.oid AND p1.proname = p2.proname AND
    p1.proisagg AND p2.proisagg AND
    array_dims(p1.proargtypes) != array_dims(p2.proargtypes)
ORDER BY 1;

-- For the same reason, built-in aggregates with default arguments are no good.

SELECT oid, proname
FROM pg_proc AS p
WHERE proisagg AND proargdefaults IS NOT NULL;

-- For the same reason, we avoid creating built-in variadic aggregates, except
-- that variadic ordered-set aggregates are OK (since they have special syntax
-- that is not subject to the misplaced ORDER BY issue).

SELECT p.oid, proname
FROM pg_proc AS p JOIN pg_aggregate AS a ON a.aggfnoid = p.oid
WHERE proisagg AND provariadic != 0 AND a.aggkind = 'n';

-- **************** pg_opfamily ****************

-- Look for illegal values in pg_opfamily fields

SELECT p1.oid
FROM pg_opfamily as p1
WHERE p1.opfmethod = 0 OR p1.opfnamespace = 0;

-- **************** pg_opclass ****************

-- Look for illegal values in pg_opclass fields

SELECT p1.oid
FROM pg_opclass AS p1
WHERE p1.opcmethod = 0 OR p1.opcnamespace = 0 OR p1.opcfamily = 0
    OR p1.opcintype = 0;

-- opcmethod must match owning opfamily's opfmethod

SELECT p1.oid, p2.oid
FROM pg_opclass AS p1, pg_opfamily AS p2
WHERE p1.opcfamily = p2.oid AND p1.opcmethod != p2.opfmethod;

-- There should not be multiple entries in pg_opclass with opcdefault true
-- and the same opcmethod/opcintype combination.

SELECT p1.oid, p2.oid
FROM pg_opclass AS p1, pg_opclass AS p2
WHERE p1.oid != p2.oid AND
    p1.opcmethod = p2.opcmethod AND p1.opcintype = p2.opcintype AND
    p1.opcdefault AND p2.opcdefault;

-- **************** pg_amop ****************

-- Look for illegal values in pg_amop fields

SELECT p1.amopfamily, p1.amopstrategy
FROM pg_amop as p1
WHERE p1.amopfamily = 0 OR p1.amoplefttype = 0 OR p1.amoprighttype = 0
    OR p1.amopopr = 0 OR p1.amopmethod = 0 OR p1.amopstrategy < 1;

SELECT p1.amopfamily, p1.amopstrategy
FROM pg_amop as p1
WHERE NOT ((p1.amoppurpose = 's' AND p1.amopsortfamily = 0) OR
           (p1.amoppurpose = 'o' AND p1.amopsortfamily <> 0));

-- amoplefttype/amoprighttype must match the operator

SELECT p1.oid, p2.oid
FROM pg_amop AS p1, pg_operator AS p2
WHERE p1.amopopr = p2.oid AND NOT
    (p1.amoplefttype = p2.oprleft AND p1.amoprighttype = p2.oprright);

-- amopmethod must match owning opfamily's opfmethod

SELECT p1.oid, p2.oid
FROM pg_amop AS p1, pg_opfamily AS p2
WHERE p1.amopfamily = p2.oid AND p1.amopmethod != p2.opfmethod;

-- amopsortfamily, if present, must reference a btree family

SELECT p1.amopfamily, p1.amopstrategy
FROM pg_amop AS p1
WHERE p1.amopsortfamily <> 0 AND NOT EXISTS
    (SELECT 1 from pg_opfamily op WHERE op.oid = p1.amopsortfamily
     AND op.opfmethod = (SELECT oid FROM pg_am WHERE amname = 'btree'));

-- check for ordering operators not supported by parent AM

SELECT p1.amopfamily, p1.amopopr, p2.oid, p2.amname
FROM pg_amop AS p1, pg_am AS p2
WHERE p1.amopmethod = p2.oid AND
    p1.amoppurpose = 'o' AND NOT p2.amcanorderbyop;

-- Cross-check amopstrategy index against parent AM

SELECT p1.amopfamily, p1.amopopr, p2.oid, p2.amname
FROM pg_amop AS p1, pg_am AS p2
WHERE p1.amopmethod = p2.oid AND
    p1.amopstrategy > p2.amstrategies AND p2.amstrategies <> 0;

-- Detect missing pg_amop entries: should have as many strategy operators
-- as AM expects for each datatype combination supported by the opfamily.
-- We can't check this for AMs with variable strategy sets.

SELECT p1.amname, p2.amoplefttype, p2.amoprighttype
FROM pg_am AS p1, pg_amop AS p2
WHERE p2.amopmethod = p1.oid AND
    p1.amstrategies <> 0 AND
    p1.amstrategies != (SELECT count(*) FROM pg_amop AS p3
                        WHERE p3.amopfamily = p2.amopfamily AND
                              p3.amoplefttype = p2.amoplefttype AND
                              p3.amoprighttype = p2.amoprighttype AND
                              p3.amoppurpose = 's');

-- Currently, none of the AMs with fixed strategy sets support ordering ops.

SELECT p1.amname, p2.amopfamily, p2.amopstrategy
FROM pg_am AS p1, pg_amop AS p2
WHERE p2.amopmethod = p1.oid AND
    p1.amstrategies <> 0 AND p2.amoppurpose <> 's';

-- Check that amopopr points at a reasonable-looking operator, ie a binary
-- operator.  If it's a search operator it had better yield boolean,
-- otherwise an input type of its sort opfamily.

SELECT p1.amopfamily, p1.amopopr, p2.oid, p2.oprname
FROM pg_amop AS p1, pg_operator AS p2
WHERE p1.amopopr = p2.oid AND
    p2.oprkind != 'b';

SELECT p1.amopfamily, p1.amopopr, p2.oid, p2.oprname
FROM pg_amop AS p1, pg_operator AS p2
WHERE p1.amopopr = p2.oid AND p1.amoppurpose = 's' AND
    p2.oprresult != 'bool'::regtype;

SELECT p1.amopfamily, p1.amopopr, p2.oid, p2.oprname
FROM pg_amop AS p1, pg_operator AS p2
WHERE p1.amopopr = p2.oid AND p1.amoppurpose = 'o' AND NOT EXISTS
    (SELECT 1 FROM pg_opclass op
     WHERE opcfamily = p1.amopsortfamily AND opcintype = p2.oprresult);

-- Make a list of all the distinct operator names being used in particular
-- strategy slots.  This is a bit hokey, since the list might need to change
-- in future releases, but it's an effective way of spotting mistakes such as
-- swapping two operators within a family.

SELECT DISTINCT amopmethod, amopstrategy, oprname
FROM pg_amop p1 LEFT JOIN pg_operator p2 ON amopopr = p2.oid
ORDER BY 1, 2, 3;

-- Check that all opclass search operators have selectivity estimators.
-- This is not absolutely required, but it seems a reasonable thing
-- to insist on for all standard datatypes.

SELECT p1.amopfamily, p1.amopopr, p2.oid, p2.oprname
FROM pg_amop AS p1, pg_operator AS p2
WHERE p1.amopopr = p2.oid AND p1.amoppurpose = 's' AND
    (p2.oprrest = 0 OR p2.oprjoin = 0);

-- Check that each opclass in an opfamily has associated operators, that is
-- ones whose oprleft matches opcintype (possibly by coercion).

SELECT p1.opcname, p1.opcfamily
FROM pg_opclass AS p1
WHERE NOT EXISTS(SELECT 1 FROM pg_amop AS p2
                 WHERE p2.amopfamily = p1.opcfamily
                   AND binary_coercible(p1.opcintype, p2.amoplefttype));

-- Check that each operator listed in pg_amop has an associated opclass,
-- that is one whose opcintype matches oprleft (possibly by coercion).
-- Otherwise the operator is useless because it cannot be matched to an index.
-- (In principle it could be useful to list such operators in multiple-datatype
-- btree opfamilies, but in practice you'd expect there to be an opclass for
-- every datatype the family knows about.)

SELECT p1.amopfamily, p1.amopstrategy, p1.amopopr
FROM pg_amop AS p1
WHERE NOT EXISTS(SELECT 1 FROM pg_opclass AS p2
                 WHERE p2.opcfamily = p1.amopfamily
                   AND binary_coercible(p2.opcintype, p1.amoplefttype));

-- Operators that are primary members of opclasses must be immutable (else
-- it suggests that the index ordering isn't fixed).  Operators that are
-- cross-type members need only be stable, since they are just shorthands
-- for index probe queries.

SELECT p1.amopfamily, p1.amopopr, p2.oprname, p3.prosrc
FROM pg_amop AS p1, pg_operator AS p2, pg_proc AS p3
WHERE p1.amopopr = p2.oid AND p2.oprcode = p3.oid AND
    p1.amoplefttype = p1.amoprighttype AND
    p3.provolatile != 'i';

SELECT p1.amopfamily, p1.amopopr, p2.oprname, p3.prosrc
FROM pg_amop AS p1, pg_operator AS p2, pg_proc AS p3
WHERE p1.amopopr = p2.oid AND p2.oprcode = p3.oid AND
    p1.amoplefttype != p1.amoprighttype AND
    p3.provolatile = 'v';

-- Multiple-datatype btree opfamilies should provide closed sets of equality
-- operators; that is if you provide int2 = int4 and int4 = int8 then you
-- should also provide int2 = int8 (and commutators of all these).  This is
-- important because the planner tries to deduce additional qual clauses from
-- transitivity of mergejoinable operators.  If there are clauses
-- int2var = int4var and int4var = int8var, the planner will want to deduce
-- int2var = int8var ... so there should be a way to represent that.  While
-- a missing cross-type operator is now only an efficiency loss rather than
-- an error condition, it still seems reasonable to insist that all built-in
-- opfamilies be complete.

-- check commutative closure
SELECT p1.amoplefttype, p1.amoprighttype
FROM pg_amop AS p1
WHERE p1.amopmethod = (SELECT oid FROM pg_am WHERE amname = 'btree') AND
    p1.amopstrategy = 3 AND
    p1.amoplefttype != p1.amoprighttype AND
    NOT EXISTS(SELECT 1 FROM pg_amop p2 WHERE
                 p2.amopfamily = p1.amopfamily AND
                 p2.amoplefttype = p1.amoprighttype AND
                 p2.amoprighttype = p1.amoplefttype AND
                 p2.amopstrategy = 3);

-- check transitive closure
SELECT p1.amoplefttype, p1.amoprighttype, p2.amoprighttype
FROM pg_amop AS p1, pg_amop AS p2
WHERE p1.amopfamily = p2.amopfamily AND
    p1.amoprighttype = p2.amoplefttype AND
    p1.amopmethod = (SELECT oid FROM pg_am WHERE amname = 'btree') AND
    p2.amopmethod = (SELECT oid FROM pg_am WHERE amname = 'btree') AND
    p1.amopstrategy = 3 AND p2.amopstrategy = 3 AND
    p1.amoplefttype != p1.amoprighttype AND
    p2.amoplefttype != p2.amoprighttype AND
    NOT EXISTS(SELECT 1 FROM pg_amop p3 WHERE
                 p3.amopfamily = p1.amopfamily AND
                 p3.amoplefttype = p1.amoplefttype AND
                 p3.amoprighttype = p2.amoprighttype AND
                 p3.amopstrategy = 3);

-- We also expect that built-in multiple-datatype hash opfamilies provide
-- complete sets of cross-type operators.  Again, this isn't required, but
-- it is reasonable to expect it for built-in opfamilies.

-- if same family has x=x and y=y, it should have x=y
SELECT p1.amoplefttype, p2.amoplefttype
FROM pg_amop AS p1, pg_amop AS p2
WHERE p1.amopfamily = p2.amopfamily AND
    p1.amoplefttype = p1.amoprighttype AND
    p2.amoplefttype = p2.amoprighttype AND
    p1.amopmethod = (SELECT oid FROM pg_am WHERE amname = 'hash') AND
    p2.amopmethod = (SELECT oid FROM pg_am WHERE amname = 'hash') AND
    p1.amopstrategy = 1 AND p2.amopstrategy = 1 AND
    p1.amoplefttype != p2.amoplefttype AND
    NOT EXISTS(SELECT 1 FROM pg_amop p3 WHERE
                 p3.amopfamily = p1.amopfamily AND
                 p3.amoplefttype = p1.amoplefttype AND
                 p3.amoprighttype = p2.amoplefttype AND
                 p3.amopstrategy = 1);


-- **************** pg_amproc ****************

-- Look for illegal values in pg_amproc fields

SELECT p1.amprocfamily, p1.amprocnum
FROM pg_amproc as p1
WHERE p1.amprocfamily = 0 OR p1.amproclefttype = 0 OR p1.amprocrighttype = 0
    OR p1.amprocnum < 1 OR p1.amproc = 0;

-- Cross-check amprocnum index against parent AM

SELECT p1.amprocfamily, p1.amprocnum, p2.oid, p2.amname
FROM pg_amproc AS p1, pg_am AS p2, pg_opfamily AS p3
WHERE p1.amprocfamily = p3.oid AND p3.opfmethod = p2.oid AND
    p1.amprocnum > p2.amsupport;

-- Detect missing pg_amproc entries: should have as many support functions
-- as AM expects for each datatype combination supported by the opfamily.

SELECT * FROM (
  SELECT p1.amname, p2.opfname, p3.amproclefttype, p3.amprocrighttype,
         array_agg(p3.amprocnum ORDER BY amprocnum) AS procnums
  FROM pg_am AS p1, pg_opfamily AS p2, pg_amproc AS p3
  WHERE p2.opfmethod = p1.oid AND p3.amprocfamily = p2.oid
  GROUP BY p1.amname, p2.opfname, p3.amproclefttype, p3.amprocrighttype
) AS t
WHERE NOT (
  -- btree has one mandatory and one optional support function.
  -- hash has one support function, which is mandatory.
  -- GiST has eight support functions, one of which is optional.
  -- GIN has six support functions. 1-3 are mandatory, 5 is optional, and
  --   at least one of 4 and 6 must be given.
  -- SP-GiST has five support functions, all mandatory
  amname = 'btree' AND procnums @> '{1}' OR
  amname = 'hash' AND procnums = '{1}' OR
  amname = 'gist' AND procnums @> '{1, 2, 3, 4, 5, 6, 7}' OR
  amname = 'gin' AND (procnums @> '{1, 2, 3}' AND (procnums && '{4, 6}')) OR
  amname = 'spgist' AND procnums = '{1, 2, 3, 4, 5}'
);

-- Also, check if there are any pg_opclass entries that don't seem to have
-- pg_amproc support.

SELECT * FROM (
  SELECT amname, opcname, array_agg(amprocnum ORDER BY amprocnum) as procnums
  FROM pg_am am JOIN pg_opclass op ON opcmethod = am.oid
       LEFT JOIN pg_amproc p ON amprocfamily = opcfamily AND
           amproclefttype = amprocrighttype AND amproclefttype = opcintype
  GROUP BY amname, opcname, amprocfamily
) AS t
WHERE NOT (
  -- same per-AM rules as above
  amname = 'btree' AND procnums @> '{1}' OR
  amname = 'hash' AND procnums = '{1}' OR
  amname = 'gist' AND procnums @> '{1, 2, 3, 4, 5, 6, 7}' OR
  amname = 'gin' AND (procnums @> '{1, 2, 3}' AND (procnums && '{4, 6}')) OR
  amname = 'spgist' AND procnums = '{1, 2, 3, 4, 5}'
);

-- Unfortunately, we can't check the amproc link very well because the
-- signature of the function may be different for different support routines
-- or different base data types.
-- We can check that all the referenced instances of the same support
-- routine number take the same number of parameters, but that's about it
-- for a general check...

SELECT p1.amprocfamily, p1.amprocnum,
	p2.oid, p2.proname,
	p3.opfname,
	p4.amprocfamily, p4.amprocnum,
	p5.oid, p5.proname,
	p6.opfname
FROM pg_amproc AS p1, pg_proc AS p2, pg_opfamily AS p3,
     pg_amproc AS p4, pg_proc AS p5, pg_opfamily AS p6
WHERE p1.amprocfamily = p3.oid AND p4.amprocfamily = p6.oid AND
    p3.opfmethod = p6.opfmethod AND p1.amprocnum = p4.amprocnum AND
    p1.amproc = p2.oid AND p4.amproc = p5.oid AND
    (p2.proretset OR p5.proretset OR p2.pronargs != p5.pronargs);

-- For btree, though, we can do better since we know the support routines
-- must be of the form cmp(lefttype, righttype) returns int4
-- or sortsupport(internal) returns void.

SELECT p1.amprocfamily, p1.amprocnum,
	p2.oid, p2.proname,
	p3.opfname
FROM pg_amproc AS p1, pg_proc AS p2, pg_opfamily AS p3
WHERE p3.opfmethod = (SELECT oid FROM pg_am WHERE amname = 'btree')
    AND p1.amprocfamily = p3.oid AND p1.amproc = p2.oid AND
    (CASE WHEN amprocnum = 1
          THEN prorettype != 'int4'::regtype OR proretset OR pronargs != 2
               OR proargtypes[0] != amproclefttype
               OR proargtypes[1] != amprocrighttype
          WHEN amprocnum = 2
          THEN prorettype != 'void'::regtype OR proretset OR pronargs != 1
               OR proargtypes[0] != 'internal'::regtype
          ELSE true END);

-- For hash we can also do a little better: the support routines must be
-- of the form hash(lefttype) returns int4.  There are several cases where
-- we cheat and use a hash function that is physically compatible with the
-- datatype even though there's no cast, so this check does find a small
-- number of entries.

SELECT p1.amprocfamily, p1.amprocnum, p2.proname, p3.opfname
FROM pg_amproc AS p1, pg_proc AS p2, pg_opfamily AS p3
WHERE p3.opfmethod = (SELECT oid FROM pg_am WHERE amname = 'hash')
    AND p1.amprocfamily = p3.oid AND p1.amproc = p2.oid AND
    (amprocnum != 1
     OR proretset
     OR prorettype != 'int4'::regtype
     OR pronargs != 1
     OR NOT physically_coercible(amproclefttype, proargtypes[0])
     OR amproclefttype != amprocrighttype)
ORDER BY 1;

-- We can also check SP-GiST carefully, since the support routine signatures
-- are independent of the datatype being indexed.

SELECT p1.amprocfamily, p1.amprocnum,
	p2.oid, p2.proname,
	p3.opfname
FROM pg_amproc AS p1, pg_proc AS p2, pg_opfamily AS p3
WHERE p3.opfmethod = (SELECT oid FROM pg_am WHERE amname = 'spgist')
    AND p1.amprocfamily = p3.oid AND p1.amproc = p2.oid AND
    (CASE WHEN amprocnum = 1 OR amprocnum = 2 OR amprocnum = 3 OR amprocnum = 4
          THEN prorettype != 'void'::regtype OR proretset OR pronargs != 2
               OR proargtypes[0] != 'internal'::regtype
               OR proargtypes[1] != 'internal'::regtype
          WHEN amprocnum = 5
          THEN prorettype != 'bool'::regtype OR proretset OR pronargs != 2
               OR proargtypes[0] != 'internal'::regtype
               OR proargtypes[1] != 'internal'::regtype
          ELSE true END);

-- Support routines that are primary members of opfamilies must be immutable
-- (else it suggests that the index ordering isn't fixed).  But cross-type
-- members need only be stable, since they are just shorthands
-- for index probe queries.

SELECT p1.amprocfamily, p1.amproc, p2.prosrc
FROM pg_amproc AS p1, pg_proc AS p2
WHERE p1.amproc = p2.oid AND
    p1.amproclefttype = p1.amprocrighttype AND
    p2.provolatile != 'i';

SELECT p1.amprocfamily, p1.amproc, p2.prosrc
FROM pg_amproc AS p1, pg_proc AS p2
WHERE p1.amproc = p2.oid AND
    p1.amproclefttype != p1.amprocrighttype AND
    p2.provolatile = 'v';
