--
-- OPR_SANITY
-- Sanity checks for common errors in making operator/procedure system tables:
-- pg_operator, pg_proc, pg_cast, pg_conversion, pg_aggregate, pg_am,
-- pg_amop, pg_amproc, pg_opclass, pg_opfamily, pg_index.
--
-- Every test failure in this file should be closely inspected.
-- The description of the failing test should be read carefully before
-- adjusting the expected output.  In most cases, the queries should
-- not find *any* matching entries.
--
-- NB: we assume the oidjoins test will have caught any dangling links,
-- that is OID or REGPROC fields that are not zero and do not match some
-- row in the linked-to table.  However, if we want to enforce that a link
-- field can't be 0, we have to check it here.
--
-- NB: run this test earlier than the create_operator test, because
-- that test creates some bogus operators...


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
       CASE WHEN proretset THEN prorows <= 0 ELSE prorows != 0 END OR
       prokind NOT IN ('f', 'a', 'w', 'p') OR
       provolatile NOT IN ('i', 's', 'v') OR
       proparallel NOT IN ('s', 'r', 'u');

-- prosrc should never be null; it can be empty only if prosqlbody isn't null
SELECT p1.oid, p1.proname
FROM pg_proc as p1
WHERE prosrc IS NULL;
SELECT p1.oid, p1.proname
FROM pg_proc as p1
WHERE (prosrc = '' OR prosrc = '-') AND prosqlbody IS NULL;

-- proretset should only be set for normal functions
SELECT p1.oid, p1.proname
FROM pg_proc AS p1
WHERE proretset AND prokind != 'f';

-- currently, no built-in functions should be SECURITY DEFINER;
-- this might change in future, but there will probably never be many.
SELECT p1.oid, p1.proname
FROM pg_proc AS p1
WHERE prosecdef
ORDER BY 1;

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
    (p1.prokind != 'a' OR p2.prokind != 'a') AND
    (p1.prolang != p2.prolang OR
     p1.prokind != p2.prokind OR
     p1.prosecdef != p2.prosecdef OR
     p1.proleakproof != p2.proleakproof OR
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
-- dummy built-in function.  Likewise, ignore range and multirange constructor
-- functions.

SELECT DISTINCT p1.prorettype::regtype, p2.prorettype::regtype
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    p1.prolang = 12 AND p2.prolang = 12 AND
    p1.prokind != 'a' AND p2.prokind != 'a' AND
    p1.prosrc NOT LIKE E'range\\_constructor_' AND
    p2.prosrc NOT LIKE E'range\\_constructor_' AND
    p1.prosrc NOT LIKE E'multirange\\_constructor_' AND
    p2.prosrc NOT LIKE E'multirange\\_constructor_' AND
    (p1.prorettype < p2.prorettype)
ORDER BY 1, 2;

SELECT DISTINCT p1.proargtypes[0]::regtype, p2.proargtypes[0]::regtype
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    p1.prolang = 12 AND p2.prolang = 12 AND
    p1.prokind != 'a' AND p2.prokind != 'a' AND
    p1.prosrc NOT LIKE E'range\\_constructor_' AND
    p2.prosrc NOT LIKE E'range\\_constructor_' AND
    p1.prosrc NOT LIKE E'multirange\\_constructor_' AND
    p2.prosrc NOT LIKE E'multirange\\_constructor_' AND
    (p1.proargtypes[0] < p2.proargtypes[0])
ORDER BY 1, 2;

SELECT DISTINCT p1.proargtypes[1]::regtype, p2.proargtypes[1]::regtype
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    p1.prolang = 12 AND p2.prolang = 12 AND
    p1.prokind != 'a' AND p2.prokind != 'a' AND
    p1.prosrc NOT LIKE E'range\\_constructor_' AND
    p2.prosrc NOT LIKE E'range\\_constructor_' AND
    p1.prosrc NOT LIKE E'multirange\\_constructor_' AND
    p2.prosrc NOT LIKE E'multirange\\_constructor_' AND
    (p1.proargtypes[1] < p2.proargtypes[1])
ORDER BY 1, 2;

SELECT DISTINCT p1.proargtypes[2]::regtype, p2.proargtypes[2]::regtype
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    p1.prolang = 12 AND p2.prolang = 12 AND
    p1.prokind != 'a' AND p2.prokind != 'a' AND
    (p1.proargtypes[2] < p2.proargtypes[2])
ORDER BY 1, 2;

SELECT DISTINCT p1.proargtypes[3]::regtype, p2.proargtypes[3]::regtype
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    p1.prolang = 12 AND p2.prolang = 12 AND
    p1.prokind != 'a' AND p2.prokind != 'a' AND
    (p1.proargtypes[3] < p2.proargtypes[3])
ORDER BY 1, 2;

SELECT DISTINCT p1.proargtypes[4]::regtype, p2.proargtypes[4]::regtype
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    p1.prolang = 12 AND p2.prolang = 12 AND
    p1.prokind != 'a' AND p2.prokind != 'a' AND
    (p1.proargtypes[4] < p2.proargtypes[4])
ORDER BY 1, 2;

SELECT DISTINCT p1.proargtypes[5]::regtype, p2.proargtypes[5]::regtype
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    p1.prolang = 12 AND p2.prolang = 12 AND
    p1.prokind != 'a' AND p2.prokind != 'a' AND
    (p1.proargtypes[5] < p2.proargtypes[5])
ORDER BY 1, 2;

SELECT DISTINCT p1.proargtypes[6]::regtype, p2.proargtypes[6]::regtype
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    p1.prolang = 12 AND p2.prolang = 12 AND
    p1.prokind != 'a' AND p2.prokind != 'a' AND
    (p1.proargtypes[6] < p2.proargtypes[6])
ORDER BY 1, 2;

SELECT DISTINCT p1.proargtypes[7]::regtype, p2.proargtypes[7]::regtype
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    p1.prolang = 12 AND p2.prolang = 12 AND
    p1.prokind != 'a' AND p2.prokind != 'a' AND
    (p1.proargtypes[7] < p2.proargtypes[7])
ORDER BY 1, 2;

-- Look for functions that return type "internal" and do not have any
-- "internal" argument.  Such a function would be a security hole since
-- it might be used to call an internal function from an SQL command.
-- As of 7.3 this query should find only internal_in, which is safe because
-- it always throws an error when called.

SELECT p1.oid, p1.proname
FROM pg_proc as p1
WHERE p1.prorettype = 'internal'::regtype AND NOT
    'internal'::regtype = ANY (p1.proargtypes);

-- Look for functions that return a polymorphic type and do not have any
-- polymorphic argument.  Calls of such functions would be unresolvable
-- at parse time.  As of 9.6 this query should find only some input functions
-- and GiST support functions associated with these pseudotypes.

SELECT p1.oid, p1.proname
FROM pg_proc as p1
WHERE p1.prorettype IN
    ('anyelement'::regtype, 'anyarray'::regtype, 'anynonarray'::regtype,
     'anyenum'::regtype)
  AND NOT
    ('anyelement'::regtype = ANY (p1.proargtypes) OR
     'anyarray'::regtype = ANY (p1.proargtypes) OR
     'anynonarray'::regtype = ANY (p1.proargtypes) OR
     'anyenum'::regtype = ANY (p1.proargtypes) OR
     'anyrange'::regtype = ANY (p1.proargtypes) OR
     'anymultirange'::regtype = ANY (p1.proargtypes))
ORDER BY 2;

-- anyrange and anymultirange are tighter than the rest, can only resolve
-- from each other

SELECT p1.oid, p1.proname
FROM pg_proc as p1
WHERE p1.prorettype IN ('anyrange'::regtype, 'anymultirange'::regtype)
  AND NOT
    ('anyrange'::regtype = ANY (p1.proargtypes) OR
      'anymultirange'::regtype = ANY (p1.proargtypes))
ORDER BY 2;

-- similarly for the anycompatible family

SELECT p1.oid, p1.proname
FROM pg_proc as p1
WHERE p1.prorettype IN
    ('anycompatible'::regtype, 'anycompatiblearray'::regtype,
     'anycompatiblenonarray'::regtype)
  AND NOT
    ('anycompatible'::regtype = ANY (p1.proargtypes) OR
     'anycompatiblearray'::regtype = ANY (p1.proargtypes) OR
     'anycompatiblenonarray'::regtype = ANY (p1.proargtypes) OR
     'anycompatiblerange'::regtype = ANY (p1.proargtypes))
ORDER BY 2;

SELECT p1.oid, p1.proname
FROM pg_proc as p1
WHERE p1.prorettype = 'anycompatiblerange'::regtype
  AND NOT
     'anycompatiblerange'::regtype = ANY (p1.proargtypes)
ORDER BY 2;


-- Look for functions that accept cstring and are neither datatype input
-- functions nor encoding conversion functions.  It's almost never a good
-- idea to use cstring input for a function meant to be called from SQL;
-- text should be used instead, because cstring lacks suitable casts.
-- As of 9.6 this query should find only cstring_out and cstring_send.
-- However, we must manually exclude shell_in, which might or might not be
-- rejected by the EXISTS clause depending on whether there are currently
-- any shell types.

SELECT p1.oid, p1.proname
FROM pg_proc as p1
WHERE 'cstring'::regtype = ANY (p1.proargtypes)
    AND NOT EXISTS(SELECT 1 FROM pg_type WHERE typinput = p1.oid)
    AND NOT EXISTS(SELECT 1 FROM pg_conversion WHERE conproc = p1.oid)
    AND p1.oid != 'shell_in(cstring)'::regprocedure
ORDER BY 1;

-- Likewise, look for functions that return cstring and aren't datatype output
-- functions nor typmod output functions.
-- As of 9.6 this query should find only cstring_in and cstring_recv.
-- However, we must manually exclude shell_out.

SELECT p1.oid, p1.proname
FROM pg_proc as p1
WHERE  p1.prorettype = 'cstring'::regtype
    AND NOT EXISTS(SELECT 1 FROM pg_type WHERE typoutput = p1.oid)
    AND NOT EXISTS(SELECT 1 FROM pg_type WHERE typmodout = p1.oid)
    AND p1.oid != 'shell_out(void)'::regprocedure
ORDER BY 1;

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

-- Check for type of the variadic array parameter's elements.
-- provariadic should be ANYOID if the type of the last element is ANYOID,
-- ANYELEMENTOID if the type of the last element is ANYARRAYOID,
-- ANYCOMPATIBLEOID if the type of the last element is ANYCOMPATIBLEARRAYOID,
-- and otherwise the element type corresponding to the array type.

SELECT oid::regprocedure, provariadic::regtype, proargtypes::regtype[]
FROM pg_proc
WHERE provariadic != 0
AND case proargtypes[array_length(proargtypes, 1)-1]
	WHEN '"any"'::regtype THEN '"any"'::regtype
	WHEN 'anyarray'::regtype THEN 'anyelement'::regtype
	WHEN 'anycompatiblearray'::regtype THEN 'anycompatible'::regtype
	ELSE (SELECT t.oid
		  FROM pg_type t
		  WHERE t.typarray = proargtypes[array_length(proargtypes, 1)-1])
	END  != provariadic;

-- Check that all and only those functions with a variadic type have
-- a variadic argument.
SELECT oid::regprocedure, proargmodes, provariadic
FROM pg_proc
WHERE (proargmodes IS NOT NULL AND 'v' = any(proargmodes))
    IS DISTINCT FROM
    (provariadic != 0);

-- Check for prosupport functions with the wrong signature
SELECT p1.oid, p1.proname, p2.oid, p2.proname
FROM pg_proc AS p1, pg_proc AS p2
WHERE p2.oid = p1.prosupport AND
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

-- Check that all immutable functions are marked parallel safe
SELECT p1.oid, p1.proname
FROM pg_proc AS p1
WHERE provolatile = 'i' AND proparallel = 'u';


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


-- **************** pg_conversion ****************

-- Look for illegal values in pg_conversion fields.

SELECT c.oid, c.conname
FROM pg_conversion as c
WHERE c.conproc = 0 OR
    pg_encoding_to_char(conforencoding) = '' OR
    pg_encoding_to_char(contoencoding) = '';

-- Look for conprocs that don't have the expected signature.

SELECT p.oid, p.proname, c.oid, c.conname
FROM pg_proc p, pg_conversion c
WHERE p.oid = c.conproc AND
    (p.prorettype != 'int4'::regtype OR p.proretset OR
     p.pronargs != 6 OR
     p.proargtypes[0] != 'int4'::regtype OR
     p.proargtypes[1] != 'int4'::regtype OR
     p.proargtypes[2] != 'cstring'::regtype OR
     p.proargtypes[3] != 'internal'::regtype OR
     p.proargtypes[4] != 'int4'::regtype OR
     p.proargtypes[5] != 'bool'::regtype);

-- Check for conprocs that don't perform the specific conversion that
-- pg_conversion alleges they do, by trying to invoke each conversion
-- on some simple ASCII data.  (The conproc should throw an error if
-- it doesn't accept the encodings that are passed to it.)
-- Unfortunately, we can't test non-default conprocs this way, because
-- there is no way to ask convert() to invoke them, and we cannot call
-- them directly from SQL.  But there are no non-default built-in
-- conversions anyway.
-- (Similarly, this doesn't cope with any search path issues.)

SELECT c.oid, c.conname
FROM pg_conversion as c
WHERE condefault AND
    convert('ABC'::bytea, pg_encoding_to_char(conforencoding),
            pg_encoding_to_char(contoencoding)) != 'ABC';


-- **************** pg_operator ****************

-- Look for illegal values in pg_operator fields.

SELECT o1.oid, o1.oprname
FROM pg_operator as o1
WHERE (o1.oprkind != 'b' AND o1.oprkind != 'l') OR
    o1.oprresult = 0 OR o1.oprcode = 0;

-- Look for missing or unwanted operand types

SELECT o1.oid, o1.oprname
FROM pg_operator as o1
WHERE (o1.oprleft = 0 and o1.oprkind != 'l') OR
    (o1.oprleft != 0 and o1.oprkind = 'l') OR
    o1.oprright = 0;

-- Look for conflicting operator definitions (same names and input datatypes).

SELECT o1.oid, o1.oprcode, o2.oid, o2.oprcode
FROM pg_operator AS o1, pg_operator AS o2
WHERE o1.oid != o2.oid AND
    o1.oprname = o2.oprname AND
    o1.oprkind = o2.oprkind AND
    o1.oprleft = o2.oprleft AND
    o1.oprright = o2.oprright;

-- Look for commutative operators that don't commute.
-- DEFINITIONAL NOTE: If A.oprcom = B, then x A y has the same result as y B x.
-- We expect that B will always say that B.oprcom = A as well; that's not
-- inherently essential, but it would be inefficient not to mark it so.

SELECT o1.oid, o1.oprcode, o2.oid, o2.oprcode
FROM pg_operator AS o1, pg_operator AS o2
WHERE o1.oprcom = o2.oid AND
    (o1.oprkind != 'b' OR
     o1.oprleft != o2.oprright OR
     o1.oprright != o2.oprleft OR
     o1.oprresult != o2.oprresult OR
     o1.oid != o2.oprcom);

-- Look for negatory operators that don't agree.
-- DEFINITIONAL NOTE: If A.oprnegate = B, then both A and B must yield
-- boolean results, and (x A y) == ! (x B y), or the equivalent for
-- single-operand operators.
-- We expect that B will always say that B.oprnegate = A as well; that's not
-- inherently essential, but it would be inefficient not to mark it so.
-- Also, A and B had better not be the same operator.

SELECT o1.oid, o1.oprcode, o2.oid, o2.oprcode
FROM pg_operator AS o1, pg_operator AS o2
WHERE o1.oprnegate = o2.oid AND
    (o1.oprkind != o2.oprkind OR
     o1.oprleft != o2.oprleft OR
     o1.oprright != o2.oprright OR
     o1.oprresult != 'bool'::regtype OR
     o2.oprresult != 'bool'::regtype OR
     o1.oid != o2.oprnegate OR
     o1.oid = o2.oid);

-- Make a list of the names of operators that are claimed to be commutator
-- pairs.  This list will grow over time, but before accepting a new entry
-- make sure you didn't link the wrong operators.

SELECT DISTINCT o1.oprname AS op1, o2.oprname AS op2
FROM pg_operator o1, pg_operator o2
WHERE o1.oprcom = o2.oid AND o1.oprname <= o2.oprname
ORDER BY 1, 2;

-- Likewise for negator pairs.

SELECT DISTINCT o1.oprname AS op1, o2.oprname AS op2
FROM pg_operator o1, pg_operator o2
WHERE o1.oprnegate = o2.oid AND o1.oprname <= o2.oprname
ORDER BY 1, 2;

-- A mergejoinable or hashjoinable operator must be binary, must return
-- boolean, and must have a commutator (itself, unless it's a cross-type
-- operator).

SELECT o1.oid, o1.oprname FROM pg_operator AS o1
WHERE (o1.oprcanmerge OR o1.oprcanhash) AND NOT
    (o1.oprkind = 'b' AND o1.oprresult = 'bool'::regtype AND o1.oprcom != 0);

-- What's more, the commutator had better be mergejoinable/hashjoinable too.

SELECT o1.oid, o1.oprname, o2.oid, o2.oprname
FROM pg_operator AS o1, pg_operator AS o2
WHERE o1.oprcom = o2.oid AND
    (o1.oprcanmerge != o2.oprcanmerge OR
     o1.oprcanhash != o2.oprcanhash);

-- Mergejoinable operators should appear as equality members of btree index
-- opfamilies.

SELECT o1.oid, o1.oprname
FROM pg_operator AS o1
WHERE o1.oprcanmerge AND NOT EXISTS
  (SELECT 1 FROM pg_amop
   WHERE amopmethod = (SELECT oid FROM pg_am WHERE amname = 'btree') AND
         amopopr = o1.oid AND amopstrategy = 3);

-- And the converse.

SELECT o1.oid, o1.oprname, p.amopfamily
FROM pg_operator AS o1, pg_amop p
WHERE amopopr = o1.oid
  AND amopmethod = (SELECT oid FROM pg_am WHERE amname = 'btree')
  AND amopstrategy = 3
  AND NOT o1.oprcanmerge;

-- Hashable operators should appear as members of hash index opfamilies.

SELECT o1.oid, o1.oprname
FROM pg_operator AS o1
WHERE o1.oprcanhash AND NOT EXISTS
  (SELECT 1 FROM pg_amop
   WHERE amopmethod = (SELECT oid FROM pg_am WHERE amname = 'hash') AND
         amopopr = o1.oid AND amopstrategy = 1);

-- And the converse.

SELECT o1.oid, o1.oprname, p.amopfamily
FROM pg_operator AS o1, pg_amop p
WHERE amopopr = o1.oid
  AND amopmethod = (SELECT oid FROM pg_am WHERE amname = 'hash')
  AND NOT o1.oprcanhash;

-- Check that each operator defined in pg_operator matches its oprcode entry
-- in pg_proc.  Easiest to do this separately for each oprkind.

SELECT o1.oid, o1.oprname, p1.oid, p1.proname
FROM pg_operator AS o1, pg_proc AS p1
WHERE o1.oprcode = p1.oid AND
    o1.oprkind = 'b' AND
    (p1.pronargs != 2
     OR NOT binary_coercible(p1.prorettype, o1.oprresult)
     OR NOT binary_coercible(o1.oprleft, p1.proargtypes[0])
     OR NOT binary_coercible(o1.oprright, p1.proargtypes[1]));

SELECT o1.oid, o1.oprname, p1.oid, p1.proname
FROM pg_operator AS o1, pg_proc AS p1
WHERE o1.oprcode = p1.oid AND
    o1.oprkind = 'l' AND
    (p1.pronargs != 1
     OR NOT binary_coercible(p1.prorettype, o1.oprresult)
     OR NOT binary_coercible(o1.oprright, p1.proargtypes[0])
     OR o1.oprleft != 0);

-- If the operator is mergejoinable or hashjoinable, its underlying function
-- should not be volatile.

SELECT o1.oid, o1.oprname, p1.oid, p1.proname
FROM pg_operator AS o1, pg_proc AS p1
WHERE o1.oprcode = p1.oid AND
    (o1.oprcanmerge OR o1.oprcanhash) AND
    p1.provolatile = 'v';

-- If oprrest is set, the operator must return boolean,
-- and it must link to a proc with the right signature
-- to be a restriction selectivity estimator.
-- The proc signature we want is: float8 proc(internal, oid, internal, int4)

SELECT o1.oid, o1.oprname, p2.oid, p2.proname
FROM pg_operator AS o1, pg_proc AS p2
WHERE o1.oprrest = p2.oid AND
    (o1.oprresult != 'bool'::regtype OR
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

SELECT o1.oid, o1.oprname, p2.oid, p2.proname
FROM pg_operator AS o1, pg_proc AS p2
WHERE o1.oprjoin = p2.oid AND
    (o1.oprkind != 'b' OR o1.oprresult != 'bool'::regtype OR
     p2.prorettype != 'float8'::regtype OR p2.proretset OR
     p2.pronargs != 5 OR
     p2.proargtypes[0] != 'internal'::regtype OR
     p2.proargtypes[1] != 'oid'::regtype OR
     p2.proargtypes[2] != 'internal'::regtype OR
     p2.proargtypes[3] != 'int2'::regtype OR
     p2.proargtypes[4] != 'internal'::regtype);

-- Insist that all built-in pg_operator entries have descriptions
SELECT o1.oid, o1.oprname
FROM pg_operator as o1 LEFT JOIN pg_description as d
     ON o1.tableoid = d.classoid and o1.oid = d.objoid and d.objsubid = 0
WHERE d.classoid IS NULL AND o1.oid <= 9999;

-- Check that operators' underlying functions have suitable comments,
-- namely 'implementation of XXX operator'.  (Note: it's not necessary to
-- put such comments into pg_proc.dat; initdb will generate them as needed.)
-- In some cases involving legacy names for operators, there are multiple
-- operators referencing the same pg_proc entry, so ignore operators whose
-- comments say they are deprecated.
-- We also have a few functions that are both operator support and meant to
-- be called directly; those should have comments matching their operator.
WITH funcdescs AS (
  SELECT p.oid as p_oid, proname, o.oid as o_oid,
    pd.description as prodesc,
    'implementation of ' || oprname || ' operator' as expecteddesc,
    od.description as oprdesc
  FROM pg_proc p JOIN pg_operator o ON oprcode = p.oid
       LEFT JOIN pg_description pd ON
         (pd.objoid = p.oid and pd.classoid = p.tableoid and pd.objsubid = 0)
       LEFT JOIN pg_description od ON
         (od.objoid = o.oid and od.classoid = o.tableoid and od.objsubid = 0)
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
    pd.description as prodesc,
    'implementation of ' || oprname || ' operator' as expecteddesc,
    od.description as oprdesc
  FROM pg_proc p JOIN pg_operator o ON oprcode = p.oid
       LEFT JOIN pg_description pd ON
         (pd.objoid = p.oid and pd.classoid = p.tableoid and pd.objsubid = 0)
       LEFT JOIN pg_description od ON
         (od.objoid = o.oid and od.classoid = o.tableoid and od.objsubid = 0)
  WHERE o.oid <= 9999
)
SELECT p_oid, proname, prodesc FROM funcdescs
  WHERE prodesc IS DISTINCT FROM expecteddesc
    AND oprdesc NOT LIKE 'deprecated%'
ORDER BY 1;

-- Operators that are commutator pairs should have identical volatility
-- and leakproofness markings on their implementation functions.
SELECT o1.oid, o1.oprcode, o2.oid, o2.oprcode
FROM pg_operator AS o1, pg_operator AS o2, pg_proc AS p1, pg_proc AS p2
WHERE o1.oprcom = o2.oid AND p1.oid = o1.oprcode AND p2.oid = o2.oprcode AND
    (p1.provolatile != p2.provolatile OR
     p1.proleakproof != p2.proleakproof);

-- Likewise for negator pairs.
SELECT o1.oid, o1.oprcode, o2.oid, o2.oprcode
FROM pg_operator AS o1, pg_operator AS o2, pg_proc AS p1, pg_proc AS p2
WHERE o1.oprnegate = o2.oid AND p1.oid = o1.oprcode AND p2.oid = o2.oprcode AND
    (p1.provolatile != p2.provolatile OR
     p1.proleakproof != p2.proleakproof);

-- Btree comparison operators' functions should have the same volatility
-- and leakproofness markings as the associated comparison support function.
SELECT pp.oid::regprocedure as proc, pp.provolatile as vp, pp.proleakproof as lp,
       po.oid::regprocedure as opr, po.provolatile as vo, po.proleakproof as lo
FROM pg_proc pp, pg_proc po, pg_operator o, pg_amproc ap, pg_amop ao
WHERE pp.oid = ap.amproc AND po.oid = o.oprcode AND o.oid = ao.amopopr AND
    ao.amopmethod = (SELECT oid FROM pg_am WHERE amname = 'btree') AND
    ao.amopfamily = ap.amprocfamily AND
    ao.amoplefttype = ap.amproclefttype AND
    ao.amoprighttype = ap.amprocrighttype AND
    ap.amprocnum = 1 AND
    (pp.provolatile != po.provolatile OR
     pp.proleakproof != po.proleakproof)
ORDER BY 1;


-- **************** pg_aggregate ****************

-- Look for illegal values in pg_aggregate fields.

SELECT ctid, aggfnoid::oid
FROM pg_aggregate as a
WHERE aggfnoid = 0 OR aggtransfn = 0 OR
    aggkind NOT IN ('n', 'o', 'h') OR
    aggnumdirectargs < 0 OR
    (aggkind = 'n' AND aggnumdirectargs > 0) OR
    aggfinalmodify NOT IN ('r', 's', 'w') OR
    aggmfinalmodify NOT IN ('r', 's', 'w') OR
    aggtranstype = 0 OR aggtransspace < 0 OR aggmtransspace < 0;

-- Make sure the matching pg_proc entry is sensible, too.

SELECT a.aggfnoid::oid, p.proname
FROM pg_aggregate as a, pg_proc as p
WHERE a.aggfnoid = p.oid AND
    (p.prokind != 'a' OR p.proretset OR p.pronargs < a.aggnumdirectargs);

-- Make sure there are no prokind = PROKIND_AGGREGATE pg_proc entries without matches.

SELECT oid, proname
FROM pg_proc as p
WHERE p.prokind = 'a' AND
    NOT EXISTS (SELECT 1 FROM pg_aggregate a WHERE a.aggfnoid = p.oid);

-- If there is no finalfn then the output type must be the transtype.

SELECT a.aggfnoid::oid, p.proname
FROM pg_aggregate as a, pg_proc as p
WHERE a.aggfnoid = p.oid AND
    a.aggfinalfn = 0 AND p.prorettype != a.aggtranstype;

-- Cross-check transfn against its entry in pg_proc.
SELECT a.aggfnoid::oid, p.proname, ptr.oid, ptr.proname
FROM pg_aggregate AS a, pg_proc AS p, pg_proc AS ptr
WHERE a.aggfnoid = p.oid AND
    a.aggtransfn = ptr.oid AND
    (ptr.proretset
     OR NOT (ptr.pronargs =
             CASE WHEN a.aggkind = 'n' THEN p.pronargs + 1
             ELSE greatest(p.pronargs - a.aggnumdirectargs, 1) + 1 END)
     OR NOT binary_coercible(ptr.prorettype, a.aggtranstype)
     OR NOT binary_coercible(a.aggtranstype, ptr.proargtypes[0])
     OR (p.pronargs > 0 AND
         NOT binary_coercible(p.proargtypes[0], ptr.proargtypes[1]))
     OR (p.pronargs > 1 AND
         NOT binary_coercible(p.proargtypes[1], ptr.proargtypes[2]))
     OR (p.pronargs > 2 AND
         NOT binary_coercible(p.proargtypes[2], ptr.proargtypes[3]))
     OR (p.pronargs > 3 AND
         NOT binary_coercible(p.proargtypes[3], ptr.proargtypes[4]))
     -- we could carry the check further, but 4 args is enough for now
     OR (p.pronargs > 4)
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
     -- we could carry the check further, but 4 args is enough for now
     OR (pfn.pronargs > 4)
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
FROM pg_aggregate as a
WHERE aggmtranstype != 0 AND
    (aggmtransfn = 0 OR aggminvtransfn = 0);

SELECT ctid, aggfnoid::oid
FROM pg_aggregate as a
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
     OR NOT binary_coercible(ptr.prorettype, a.aggmtranstype)
     OR NOT binary_coercible(a.aggmtranstype, ptr.proargtypes[0])
     OR (p.pronargs > 0 AND
         NOT binary_coercible(p.proargtypes[0], ptr.proargtypes[1]))
     OR (p.pronargs > 1 AND
         NOT binary_coercible(p.proargtypes[1], ptr.proargtypes[2]))
     OR (p.pronargs > 2 AND
         NOT binary_coercible(p.proargtypes[2], ptr.proargtypes[3]))
     -- we could carry the check further, but 3 args is enough for now
     OR (p.pronargs > 3)
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
     OR NOT binary_coercible(ptr.prorettype, a.aggmtranstype)
     OR NOT binary_coercible(a.aggmtranstype, ptr.proargtypes[0])
     OR (p.pronargs > 0 AND
         NOT binary_coercible(p.proargtypes[0], ptr.proargtypes[1]))
     OR (p.pronargs > 1 AND
         NOT binary_coercible(p.proargtypes[1], ptr.proargtypes[2]))
     OR (p.pronargs > 2 AND
         NOT binary_coercible(p.proargtypes[2], ptr.proargtypes[3]))
     -- we could carry the check further, but 3 args is enough for now
     OR (p.pronargs > 3)
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
     -- we could carry the check further, but 4 args is enough for now
     OR (pfn.pronargs > 4)
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

-- Check that all combine functions have signature
-- combine(transtype, transtype) returns transtype

SELECT a.aggfnoid, p.proname
FROM pg_aggregate as a, pg_proc as p
WHERE a.aggcombinefn = p.oid AND
    (p.pronargs != 2 OR
     p.prorettype != p.proargtypes[0] OR
     p.prorettype != p.proargtypes[1] OR
     NOT binary_coercible(a.aggtranstype, p.proargtypes[0]));

-- Check that no combine function for an INTERNAL transtype is strict.

SELECT a.aggfnoid, p.proname
FROM pg_aggregate as a, pg_proc as p
WHERE a.aggcombinefn = p.oid AND
    a.aggtranstype = 'internal'::regtype AND p.proisstrict;

-- serialize/deserialize functions should be specified only for aggregates
-- with transtype internal and a combine function, and we should have both
-- or neither of them.

SELECT aggfnoid, aggtranstype, aggserialfn, aggdeserialfn
FROM pg_aggregate
WHERE (aggserialfn != 0 OR aggdeserialfn != 0)
  AND (aggtranstype != 'internal'::regtype OR aggcombinefn = 0 OR
       aggserialfn = 0 OR aggdeserialfn = 0);

-- Check that all serialization functions have signature
-- serialize(internal) returns bytea
-- Also insist that they be strict; it's wasteful to run them on NULLs.

SELECT a.aggfnoid, p.proname
FROM pg_aggregate as a, pg_proc as p
WHERE a.aggserialfn = p.oid AND
    (p.prorettype != 'bytea'::regtype OR p.pronargs != 1 OR
     p.proargtypes[0] != 'internal'::regtype OR
     NOT p.proisstrict);

-- Check that all deserialization functions have signature
-- deserialize(bytea, internal) returns internal
-- Also insist that they be strict; it's wasteful to run them on NULLs.

SELECT a.aggfnoid, p.proname
FROM pg_aggregate as a, pg_proc as p
WHERE a.aggdeserialfn = p.oid AND
    (p.prorettype != 'internal'::regtype OR p.pronargs != 2 OR
     p.proargtypes[0] != 'bytea'::regtype OR
     p.proargtypes[1] != 'internal'::regtype OR
     NOT p.proisstrict);

-- Check that aggregates which have the same transition function also have
-- the same combine, serialization, and deserialization functions.
-- While that isn't strictly necessary, it's fishy if they don't.

SELECT a.aggfnoid, a.aggcombinefn, a.aggserialfn, a.aggdeserialfn,
       b.aggfnoid, b.aggcombinefn, b.aggserialfn, b.aggdeserialfn
FROM
    pg_aggregate a, pg_aggregate b
WHERE
    a.aggfnoid < b.aggfnoid AND a.aggtransfn = b.aggtransfn AND
    (a.aggcombinefn != b.aggcombinefn OR a.aggserialfn != b.aggserialfn
     OR a.aggdeserialfn != b.aggdeserialfn);

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
    p1.prokind = 'a' AND p2.prokind = 'a' AND
    array_dims(p1.proargtypes) != array_dims(p2.proargtypes)
ORDER BY 1;

-- For the same reason, built-in aggregates with default arguments are no good.

SELECT oid, proname
FROM pg_proc AS p
WHERE prokind = 'a' AND proargdefaults IS NOT NULL;

-- For the same reason, we avoid creating built-in variadic aggregates, except
-- that variadic ordered-set aggregates are OK (since they have special syntax
-- that is not subject to the misplaced ORDER BY issue).

SELECT p.oid, proname
FROM pg_proc AS p JOIN pg_aggregate AS a ON a.aggfnoid = p.oid
WHERE prokind = 'a' AND provariadic != 0 AND a.aggkind = 'n';


-- **************** pg_opfamily ****************

-- Look for illegal values in pg_opfamily fields

SELECT f.oid
FROM pg_opfamily as f
WHERE f.opfmethod = 0 OR f.opfnamespace = 0;

-- Look for opfamilies having no opclasses.  While most validation of
-- opfamilies is now handled by AM-specific amvalidate functions, that's
-- driven from pg_opclass entries below, so an empty opfamily would not
-- get noticed.

SELECT oid, opfname FROM pg_opfamily f
WHERE NOT EXISTS (SELECT 1 FROM pg_opclass WHERE opcfamily = f.oid);


-- **************** pg_opclass ****************

-- Look for illegal values in pg_opclass fields

SELECT c1.oid
FROM pg_opclass AS c1
WHERE c1.opcmethod = 0 OR c1.opcnamespace = 0 OR c1.opcfamily = 0
    OR c1.opcintype = 0;

-- opcmethod must match owning opfamily's opfmethod

SELECT c1.oid, f1.oid
FROM pg_opclass AS c1, pg_opfamily AS f1
WHERE c1.opcfamily = f1.oid AND c1.opcmethod != f1.opfmethod;

-- There should not be multiple entries in pg_opclass with opcdefault true
-- and the same opcmethod/opcintype combination.

SELECT c1.oid, c2.oid
FROM pg_opclass AS c1, pg_opclass AS c2
WHERE c1.oid != c2.oid AND
    c1.opcmethod = c2.opcmethod AND c1.opcintype = c2.opcintype AND
    c1.opcdefault AND c2.opcdefault;

-- Ask access methods to validate opclasses
-- (this replaces a lot of SQL-level checks that used to be done in this file)

SELECT oid, opcname FROM pg_opclass WHERE NOT amvalidate(oid);


-- **************** pg_am ****************

-- Look for illegal values in pg_am fields

SELECT a1.oid, a1.amname
FROM pg_am AS a1
WHERE a1.amhandler = 0;

-- Check for index amhandler functions with the wrong signature

SELECT a1.oid, a1.amname, p1.oid, p1.proname
FROM pg_am AS a1, pg_proc AS p1
WHERE p1.oid = a1.amhandler AND a1.amtype = 'i' AND
    (p1.prorettype != 'index_am_handler'::regtype
     OR p1.proretset
     OR p1.pronargs != 1
     OR p1.proargtypes[0] != 'internal'::regtype);

-- Check for table amhandler functions with the wrong signature

SELECT a1.oid, a1.amname, p1.oid, p1.proname
FROM pg_am AS a1, pg_proc AS p1
WHERE p1.oid = a1.amhandler AND a1.amtype = 's' AND
    (p1.prorettype != 'table_am_handler'::regtype
     OR p1.proretset
     OR p1.pronargs != 1
     OR p1.proargtypes[0] != 'internal'::regtype);

-- **************** pg_amop ****************

-- Look for illegal values in pg_amop fields

SELECT a1.amopfamily, a1.amopstrategy
FROM pg_amop as a1
WHERE a1.amopfamily = 0 OR a1.amoplefttype = 0 OR a1.amoprighttype = 0
    OR a1.amopopr = 0 OR a1.amopmethod = 0 OR a1.amopstrategy < 1;

SELECT a1.amopfamily, a1.amopstrategy
FROM pg_amop as a1
WHERE NOT ((a1.amoppurpose = 's' AND a1.amopsortfamily = 0) OR
           (a1.amoppurpose = 'o' AND a1.amopsortfamily <> 0));

-- amopmethod must match owning opfamily's opfmethod

SELECT a1.oid, f1.oid
FROM pg_amop AS a1, pg_opfamily AS f1
WHERE a1.amopfamily = f1.oid AND a1.amopmethod != f1.opfmethod;

-- Make a list of all the distinct operator names being used in particular
-- strategy slots.  This is a bit hokey, since the list might need to change
-- in future releases, but it's an effective way of spotting mistakes such as
-- swapping two operators within a family.

SELECT DISTINCT amopmethod, amopstrategy, oprname
FROM pg_amop a1 LEFT JOIN pg_operator o1 ON amopopr = o1.oid
ORDER BY 1, 2, 3;

-- Check that all opclass search operators have selectivity estimators.
-- This is not absolutely required, but it seems a reasonable thing
-- to insist on for all standard datatypes.

SELECT a1.amopfamily, a1.amopopr, o1.oid, o1.oprname
FROM pg_amop AS a1, pg_operator AS o1
WHERE a1.amopopr = o1.oid AND a1.amoppurpose = 's' AND
    (o1.oprrest = 0 OR o1.oprjoin = 0);

-- Check that each opclass in an opfamily has associated operators, that is
-- ones whose oprleft matches opcintype (possibly by coercion).

SELECT c1.opcname, c1.opcfamily
FROM pg_opclass AS c1
WHERE NOT EXISTS(SELECT 1 FROM pg_amop AS a1
                 WHERE a1.amopfamily = c1.opcfamily
                   AND binary_coercible(c1.opcintype, a1.amoplefttype));

-- Check that each operator listed in pg_amop has an associated opclass,
-- that is one whose opcintype matches oprleft (possibly by coercion).
-- Otherwise the operator is useless because it cannot be matched to an index.
-- (In principle it could be useful to list such operators in multiple-datatype
-- btree opfamilies, but in practice you'd expect there to be an opclass for
-- every datatype the family knows about.)

SELECT a1.amopfamily, a1.amopstrategy, a1.amopopr
FROM pg_amop AS a1
WHERE NOT EXISTS(SELECT 1 FROM pg_opclass AS c1
                 WHERE c1.opcfamily = a1.amopfamily
                   AND binary_coercible(c1.opcintype, a1.amoplefttype));

-- Operators that are primary members of opclasses must be immutable (else
-- it suggests that the index ordering isn't fixed).  Operators that are
-- cross-type members need only be stable, since they are just shorthands
-- for index probe queries.

SELECT a1.amopfamily, a1.amopopr, o1.oprname, p1.prosrc
FROM pg_amop AS a1, pg_operator AS o1, pg_proc AS p1
WHERE a1.amopopr = o1.oid AND o1.oprcode = p1.oid AND
    a1.amoplefttype = a1.amoprighttype AND
    p1.provolatile != 'i';

SELECT a1.amopfamily, a1.amopopr, o1.oprname, p1.prosrc
FROM pg_amop AS a1, pg_operator AS o1, pg_proc AS p1
WHERE a1.amopopr = o1.oid AND o1.oprcode = p1.oid AND
    a1.amoplefttype != a1.amoprighttype AND
    p1.provolatile = 'v';


-- **************** pg_amproc ****************

-- Look for illegal values in pg_amproc fields

SELECT a1.amprocfamily, a1.amprocnum
FROM pg_amproc as a1
WHERE a1.amprocfamily = 0 OR a1.amproclefttype = 0 OR a1.amprocrighttype = 0
    OR a1.amprocnum < 0 OR a1.amproc = 0;

-- Support routines that are primary members of opfamilies must be immutable
-- (else it suggests that the index ordering isn't fixed).  But cross-type
-- members need only be stable, since they are just shorthands
-- for index probe queries.

SELECT a1.amprocfamily, a1.amproc, p1.prosrc
FROM pg_amproc AS a1, pg_proc AS p1
WHERE a1.amproc = p1.oid AND
    a1.amproclefttype = a1.amprocrighttype AND
    p1.provolatile != 'i';

SELECT a1.amprocfamily, a1.amproc, p1.prosrc
FROM pg_amproc AS a1, pg_proc AS p1
WHERE a1.amproc = p1.oid AND
    a1.amproclefttype != a1.amprocrighttype AND
    p1.provolatile = 'v';

-- Almost all of the core distribution's Btree opclasses can use one of the
-- two generic "equalimage" functions as their support function 4.  Look for
-- opclasses that don't allow deduplication unconditionally here.
--
-- Newly added Btree opclasses don't have to support deduplication.  It will
-- usually be trivial to add support, though.  Note that the expected output
-- of this part of the test will need to be updated when a new opclass cannot
-- support deduplication (by using btequalimage).
SELECT amp.amproc::regproc AS proc, opf.opfname AS opfamily_name,
       opc.opcname AS opclass_name, opc.opcintype::regtype AS opcintype
FROM pg_am AS am
JOIN pg_opclass AS opc ON opc.opcmethod = am.oid
JOIN pg_opfamily AS opf ON opc.opcfamily = opf.oid
LEFT JOIN pg_amproc AS amp ON amp.amprocfamily = opf.oid AND
    amp.amproclefttype = opc.opcintype AND amp.amprocnum = 4
WHERE am.amname = 'btree' AND
    amp.amproc IS DISTINCT FROM 'btequalimage'::regproc
ORDER BY 1, 2, 3;

-- **************** pg_index ****************

-- Look for illegal values in pg_index fields.

SELECT indexrelid, indrelid
FROM pg_index
WHERE indexrelid = 0 OR indrelid = 0 OR
      indnatts <= 0 OR indnatts > 32;

-- oidvector and int2vector fields should be of length indnatts.

SELECT indexrelid, indrelid
FROM pg_index
WHERE array_lower(indkey, 1) != 0 OR array_upper(indkey, 1) != indnatts-1 OR
    array_lower(indclass, 1) != 0 OR array_upper(indclass, 1) != indnatts-1 OR
    array_lower(indcollation, 1) != 0 OR array_upper(indcollation, 1) != indnatts-1 OR
    array_lower(indoption, 1) != 0 OR array_upper(indoption, 1) != indnatts-1;

-- Check that opclasses and collations match the underlying columns.
-- (As written, this test ignores expression indexes.)

SELECT indexrelid::regclass, indrelid::regclass, attname, atttypid::regtype, opcname
FROM (SELECT indexrelid, indrelid, unnest(indkey) as ikey,
             unnest(indclass) as iclass, unnest(indcollation) as icoll
      FROM pg_index) ss,
      pg_attribute a,
      pg_opclass opc
WHERE a.attrelid = indrelid AND a.attnum = ikey AND opc.oid = iclass AND
      (NOT binary_coercible(atttypid, opcintype) OR icoll != attcollation);

-- For system catalogs, be even tighter: nearly all indexes should be
-- exact type matches not binary-coercible matches.  At this writing
-- the only exception is an OID index on a regproc column.

SELECT indexrelid::regclass, indrelid::regclass, attname, atttypid::regtype, opcname
FROM (SELECT indexrelid, indrelid, unnest(indkey) as ikey,
             unnest(indclass) as iclass, unnest(indcollation) as icoll
      FROM pg_index
      WHERE indrelid < 16384) ss,
      pg_attribute a,
      pg_opclass opc
WHERE a.attrelid = indrelid AND a.attnum = ikey AND opc.oid = iclass AND
      (opcintype != atttypid OR icoll != attcollation)
ORDER BY 1;

-- Check for system catalogs with collation-sensitive ordering.  This is not
-- a representational error in pg_index, but simply wrong catalog design.
-- It's bad because we expect to be able to clone template0 and assign the
-- copy a different database collation.  It would especially not work for
-- shared catalogs.

SELECT relname, attname, attcollation
FROM pg_class c, pg_attribute a
WHERE c.oid = attrelid AND c.oid < 16384 AND
    c.relkind != 'v' AND  -- we don't care about columns in views
    attcollation != 0 AND
    attcollation != (SELECT oid FROM pg_collation WHERE collname = 'C');

-- Double-check that collation-sensitive indexes have "C" collation, too.

SELECT indexrelid::regclass, indrelid::regclass, iclass, icoll
FROM (SELECT indexrelid, indrelid,
             unnest(indclass) as iclass, unnest(indcollation) as icoll
      FROM pg_index
      WHERE indrelid < 16384) ss
WHERE icoll != 0 AND
    icoll != (SELECT oid FROM pg_collation WHERE collname = 'C');
