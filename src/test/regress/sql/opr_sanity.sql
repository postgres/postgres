--
-- OPR_SANITY
-- Sanity checks for common errors in making operator/procedure system tables:
-- pg_operator, pg_proc, pg_aggregate, pg_am, pg_amop, pg_amproc, pg_opclass.
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
--
-- NOTE hardwired assumptions about standard types:
--                type bool has OID 16
--                type float8 has OID 701
--

-- **************** pg_proc ****************

-- Look for illegal values in pg_proc fields.
-- NOTE: currently there are a few pg_proc entries that have prorettype = 0.
-- Someday that ought to be cleaned up.

SELECT p1.oid, p1.proname
FROM pg_proc as p1
WHERE (p1.prolang = 0 OR p1.prorettype = 0 OR
    p1.pronargs < 0 OR p1.pronargs > 9)
	AND p1.proname !~ '^pl[^_]+_call_handler$'
	AND p1.proname !~ '^RI_FKey_'
	AND p1.proname !~ 'costestimate$'
	AND p1.proname != 'update_pg_pwd';

-- Look for conflicting proc definitions (same names and input datatypes).
-- (This test should be dead code now that we have the unique index
-- pg_proc_proname_narg_type_index, but I'll leave it in anyway.)

SELECT p1.oid, p1.proname, p2.oid, p2.proname
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.proname = p2.proname AND
    p1.pronargs = p2.pronargs AND
    p1.proargtypes = p2.proargtypes;

-- Considering only built-in procs (prolang = 11/12), look for multiple uses
-- of the same internal function (ie, matching prosrc fields).  It's OK to
-- have several entries with different pronames for the same internal function,
-- but conflicts in the number of arguments and other critical items should
-- be complained of.

SELECT p1.oid, p1.proname, p2.oid, p2.proname
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    (p1.prolang = 11 OR p1.prolang = 12) AND
    (p2.prolang = 11 OR p2.prolang = 12) AND
    (p1.prolang != p2.prolang OR
     p1.proisinh != p2.proisinh OR
     p1.proistrusted != p2.proistrusted OR
     p1.proiscachable != p2.proiscachable OR
     p1.pronargs != p2.pronargs OR
     p1.proretset != p2.proretset);

-- Look for uses of different type OIDs in the argument/result type fields
-- for different aliases of the same built-in function.
-- This indicates that the types are being presumed to be binary-equivalent.
-- That's not wrong, necessarily, but we make lists of all the types being
-- so treated.  Note that the expected output of this part of the test will
-- need to be modified whenever new pairs of types are made binary-equivalent!

SELECT DISTINCT p1.prorettype, p2.prorettype
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    (p1.prolang = 11 OR p1.prolang = 12) AND
    (p2.prolang = 11 OR p2.prolang = 12) AND
    (p1.prorettype < p2.prorettype);

SELECT DISTINCT p1.proargtypes[0], p2.proargtypes[0]
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    (p1.prolang = 11 OR p1.prolang = 12) AND
    (p2.prolang = 11 OR p2.prolang = 12) AND
    (p1.proargtypes[0] < p2.proargtypes[0]);

SELECT DISTINCT p1.proargtypes[1], p2.proargtypes[1]
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    (p1.prolang = 11 OR p1.prolang = 12) AND
    (p2.prolang = 11 OR p2.prolang = 12) AND
    (p1.proargtypes[1] < p2.proargtypes[1]);

SELECT DISTINCT p1.proargtypes[2], p2.proargtypes[2]
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    (p1.prolang = 11 OR p1.prolang = 12) AND
    (p2.prolang = 11 OR p2.prolang = 12) AND
    (p1.proargtypes[2] < p2.proargtypes[2]);

SELECT DISTINCT p1.proargtypes[3], p2.proargtypes[3]
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    (p1.prolang = 11 OR p1.prolang = 12) AND
    (p2.prolang = 11 OR p2.prolang = 12) AND
    (p1.proargtypes[3] < p2.proargtypes[3]);

SELECT DISTINCT p1.proargtypes[4], p2.proargtypes[4]
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    (p1.prolang = 11 OR p1.prolang = 12) AND
    (p2.prolang = 11 OR p2.prolang = 12) AND
    (p1.proargtypes[4] < p2.proargtypes[4]);

SELECT DISTINCT p1.proargtypes[5], p2.proargtypes[5]
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    (p1.prolang = 11 OR p1.prolang = 12) AND
    (p2.prolang = 11 OR p2.prolang = 12) AND
    (p1.proargtypes[5] < p2.proargtypes[5]);

SELECT DISTINCT p1.proargtypes[6], p2.proargtypes[6]
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    (p1.prolang = 11 OR p1.prolang = 12) AND
    (p2.prolang = 11 OR p2.prolang = 12) AND
    (p1.proargtypes[6] < p2.proargtypes[6]);

SELECT DISTINCT p1.proargtypes[7], p2.proargtypes[7]
FROM pg_proc AS p1, pg_proc AS p2
WHERE p1.oid != p2.oid AND
    p1.prosrc = p2.prosrc AND
    (p1.prolang = 11 OR p1.prolang = 12) AND
    (p2.prolang = 11 OR p2.prolang = 12) AND
    (p1.proargtypes[7] < p2.proargtypes[7]);

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

SELECT p1.oid, p1.oprcode, p2.oid, p2.oprcode
FROM pg_operator AS p1, pg_operator AS p2
WHERE p1.oprnegate = p2.oid AND
    (p1.oprkind != p2.oprkind OR
     p1.oprleft != p2.oprleft OR
     p1.oprright != p2.oprright OR
     p1.oprresult != 16 OR
     p2.oprresult != 16 OR
     p1.oid != p2.oprnegate);

-- Look for mergejoin operators that don't match their links.
-- A mergejoin link leads from an '=' operator to the
-- sort operator ('<' operator) that's appropriate for
-- its left-side or right-side data type.

SELECT p1.oid, p1.oprcode, p2.oid, p2.oprcode
FROM pg_operator AS p1, pg_operator AS p2
WHERE p1.oprlsortop = p2.oid AND
    (p1.oprname != '=' OR p2.oprname != '<' OR
     p1.oprkind != 'b' OR p2.oprkind != 'b' OR
     p1.oprleft != p2.oprleft OR
     p1.oprleft != p2.oprright OR
     p1.oprresult != 16 OR
     p2.oprresult != 16 OR
     p1.oprrsortop = 0);

SELECT p1.oid, p1.oprcode, p2.oid, p2.oprcode
FROM pg_operator AS p1, pg_operator AS p2
WHERE p1.oprrsortop = p2.oid AND
    (p1.oprname != '=' OR p2.oprname != '<' OR
     p1.oprkind != 'b' OR p2.oprkind != 'b' OR
     p1.oprright != p2.oprleft OR
     p1.oprright != p2.oprright OR
     p1.oprresult != 16 OR
     p2.oprresult != 16 OR
     p1.oprlsortop = 0);

-- A mergejoinable = operator must have a commutator (usually itself)
-- as well as corresponding < and > operators.  Note that the "corresponding"
-- operators have the same L and R input datatypes as the = operator,
-- whereas the operators linked to by oprlsortop and oprrsortop have input
-- datatypes L,L and R,R respectively.

SELECT p1.oid, p1.oprname FROM pg_operator AS p1
WHERE p1.oprlsortop != 0 AND
      p1.oprcom = 0;

SELECT p1.oid, p1.oprname FROM pg_operator AS p1
WHERE p1.oprlsortop != 0 AND NOT
      EXISTS(SELECT * FROM pg_operator AS p2 WHERE
        p2.oprname = '<' AND
        p2.oprleft = p1.oprleft AND
        p2.oprright = p1.oprright AND
        p2.oprkind = 'b');

SELECT p1.oid, p1.oprname FROM pg_operator AS p1
WHERE p1.oprlsortop != 0 AND NOT
      EXISTS(SELECT * FROM pg_operator AS p2 WHERE
        p2.oprname = '>' AND
        p2.oprleft = p1.oprleft AND
        p2.oprright = p1.oprright AND
        p2.oprkind = 'b');

-- Hashing only works on simple equality operators "type = sametype",
-- since the hash itself depends on the bitwise representation of the type.
-- Check that allegedly hashable operators look like they might be "=".
-- NOTE: in 6.5, this search finds int4eqoid and oideqint4.  Until we have
-- some cleaner way of dealing with binary-equivalent types, just leave
-- those two tuples in the expected output.

SELECT p1.oid, p1.oprname
FROM pg_operator AS p1
WHERE p1.oprcanhash AND NOT
    (p1.oprkind = 'b' AND p1.oprresult = 16 AND p1.oprleft = p1.oprright AND
     p1.oprname = '=' AND p1.oprcom = p1.oid);

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

-- Check that each operator defined in pg_operator matches its oprcode entry
-- in pg_proc.  Easiest to do this separately for each oprkind.
-- FIXME: want to check that argument/result types match, but how to do that
-- in the face of binary-compatible types?

SELECT p1.oid, p1.oprname, p2.oid, p2.proname
FROM pg_operator AS p1, pg_proc AS p2
WHERE p1.oprcode = p2.oid AND
    p1.oprkind = 'b' AND
    (p2.pronargs != 2
-- diked out until we find a way of marking binary-compatible types
-- OR
--     p1.oprresult != p2.prorettype OR
--     (p1.oprleft != p2.proargtypes[0] AND p2.proargtypes[0] != 0) OR
--     (p1.oprright != p2.proargtypes[1] AND p2.proargtypes[1] != 0)
);

-- These two selects can be left as-is because there are no binary-compatible
-- cases that they trip over, at least in 6.5:

SELECT p1.oid, p1.oprname, p2.oid, p2.proname
FROM pg_operator AS p1, pg_proc AS p2
WHERE p1.oprcode = p2.oid AND
    p1.oprkind = 'l' AND
    (p2.pronargs != 1 OR
     p1.oprresult != p2.prorettype OR
     (p1.oprright != p2.proargtypes[0] AND p2.proargtypes[0] != 0) OR
     p1.oprleft != 0);

SELECT p1.oid, p1.oprname, p2.oid, p2.proname
FROM pg_operator AS p1, pg_proc AS p2
WHERE p1.oprcode = p2.oid AND
    p1.oprkind = 'r' AND
    (p2.pronargs != 1 OR
     p1.oprresult != p2.prorettype OR
     (p1.oprleft != p2.proargtypes[0] AND p2.proargtypes[0] != 0) OR
     p1.oprright != 0);

-- If oprrest is set, the operator must return boolean,
-- and it must link to a proc with the right signature
-- to be a restriction selectivity estimator.
-- The proc signature we want is: float8 proc(oid, oid, int2, <any>, int4)

SELECT p1.oid, p1.oprname, p2.oid, p2.proname
FROM pg_operator AS p1, pg_proc AS p2
WHERE p1.oprrest = p2.oid AND
    (p1.oprresult != 16 OR
     p2.prorettype != 701 OR p2.proretset OR
     p2.pronargs != 5 OR
     p2.proargtypes[0] != 26 OR p2.proargtypes[1] != 26 OR
     p2.proargtypes[2] != 21 OR p2.proargtypes[3] != 0 OR
     p2.proargtypes[4] != 23);

-- If oprjoin is set, the operator must be a binary boolean op,
-- and it must link to a proc with the right signature
-- to be a join selectivity estimator.
-- The proc signature we want is: float8 proc(oid, oid, int2, oid, int2)

SELECT p1.oid, p1.oprname, p2.oid, p2.proname
FROM pg_operator AS p1, pg_proc AS p2
WHERE p1.oprjoin = p2.oid AND
    (p1.oprkind != 'b' OR p1.oprresult != 16 OR
     p2.prorettype != 701 OR p2.proretset OR
     p2.pronargs != 5 OR
     p2.proargtypes[0] != 26 OR p2.proargtypes[1] != 26 OR
     p2.proargtypes[2] != 21 OR p2.proargtypes[3] != 26 OR
     p2.proargtypes[4] != 21);

-- **************** pg_aggregate ****************

-- Look for illegal values in pg_aggregate fields.
-- aggbasetype can only be 0 if transfn1 is not present (eg, count(*))
-- or itself takes a wild-card input; we check the latter case below.

SELECT p1.oid, p1.aggname
FROM pg_aggregate as p1
WHERE (p1.aggbasetype = 0 AND p1.aggtransfn1 != 0) OR aggfinaltype = 0;

-- Check combinations of transfer functions.
-- Although either transfn1 or transfn2 can be null,
-- it makes no sense for both to be.  And if both are defined,
-- presumably there should be a finalfn to combine their results.
-- We also check that transtypes are null just when corresponding
-- transfns are.  Also, if there is no finalfn then the output type
-- must be the transtype the result will be taken from.

SELECT p1.oid, p1.aggname
FROM pg_aggregate as p1
WHERE p1.aggtransfn1 = 0 AND p1.aggtransfn2 = 0;

SELECT p1.oid, p1.aggname
FROM pg_aggregate as p1
WHERE p1.aggtransfn1 != 0 AND p1.aggtransfn2 = 0 AND
    (p1.aggtranstype1 = 0 OR p1.aggtranstype2 != 0 OR
     (p1.aggfinalfn = 0 AND p1.aggfinaltype != p1.aggtranstype1));

SELECT p1.oid, p1.aggname
FROM pg_aggregate as p1
WHERE p1.aggtransfn1 = 0 AND p1.aggtransfn2 != 0 AND
    (p1.aggtranstype1 != 0 OR p1.aggtranstype2 = 0 OR
     (p1.aggfinalfn = 0 AND p1.aggfinaltype != p1.aggtranstype2));

SELECT p1.oid, p1.aggname
FROM pg_aggregate as p1
WHERE p1.aggtransfn1 != 0 AND p1.aggtransfn2 != 0 AND
    (p1.aggtranstype1 = 0 OR p1.aggtranstype2 = 0 OR
     p1.aggfinalfn = 0);

-- Cross-check transfn1 (if present) against its entry in pg_proc.
-- FIXME: what about binary-compatible types?

SELECT p1.oid, p1.aggname, p2.oid, p2.proname
FROM pg_aggregate AS p1, pg_proc AS p2
WHERE p1.aggtransfn1 = p2.oid AND
    (p2.proretset OR p2.pronargs != 2
-- diked out until we find a way of marking binary-compatible types
-- OR
--     p1.aggtranstype1 != p2.prorettype OR
--     p1.aggtranstype1 != p2.proargtypes[0] OR
--     p1.aggbasetype != p2.proargtypes[1]
);

-- Cross-check transfn2 (if present) against its entry in pg_proc.
-- FIXME: what about binary-compatible types?

SELECT p1.oid, p1.aggname, p2.oid, p2.proname
FROM pg_aggregate AS p1, pg_proc AS p2
WHERE p1.aggtransfn2 = p2.oid AND
    (p2.proretset OR p1.aggtranstype2 != p2.prorettype OR
     p2.pronargs != 1 OR
     p1.aggtranstype2 != p2.proargtypes[0]);

-- Cross-check finalfn (if present) against its entry in pg_proc.
-- FIXME: what about binary-compatible types?

SELECT p1.oid, p1.aggname, p2.oid, p2.proname
FROM pg_aggregate AS p1, pg_proc AS p2
WHERE p1.aggfinalfn = p2.oid AND
    (p2.proretset OR p1.aggfinaltype != p2.prorettype OR
     p2.pronargs != 2 OR
     p1.aggtranstype1 != p2.proargtypes[0] OR
     p1.aggtranstype2 != p2.proargtypes[1]);

-- **************** pg_opclass ****************

-- There should not be multiple entries in pg_opclass with the same
-- nonzero opcdeftype value, because there can be only one default opclass
-- for a datatype.  (But multiple entries with zero opcdeftype are OK.)

SELECT p1.oid, p2.oid
FROM pg_opclass AS p1, pg_opclass AS p2
WHERE p1.oid != p2.oid AND
    p1.opcdeftype = p2.opcdeftype AND
    p1.opcdeftype != 0;

-- **************** pg_amop ****************

-- Look for illegal values in pg_amop fields

SELECT p1.oid
FROM pg_amop as p1
WHERE p1.amopid = 0 OR p1.amopclaid = 0 OR p1.amopopr = 0 OR
    p1.amopstrategy <= 0;

-- Look for duplicate pg_amop entries

SELECT p1.oid, p2.oid
FROM pg_amop AS p1, pg_amop AS p2
WHERE p1.oid != p2.oid AND
    p1.amopid = p2.amopid AND
    p1.amopclaid = p2.amopclaid AND
    p1.amopstrategy = p2.amopstrategy;

-- Cross-check amopstrategy index against parent AM

SELECT p1.oid, p2.oid, p2.amname
FROM pg_amop AS p1, pg_am AS p2
WHERE p1.amopid = p2.oid AND p1.amopstrategy > p2.amstrategies;

-- Detect missing pg_amop entries: should have as many strategy functions
-- as AM expects for each opclass, unless there are none at all
-- (some opclasses only offer support for a limited set of AMs...)

SELECT p1.oid, p1.amname, p2.oid, p2.opcname
FROM pg_am AS p1, pg_opclass AS p2
WHERE p1.amstrategies != (SELECT count(*) FROM pg_amop AS p3
                          WHERE p3.amopid = p1.oid AND p3.amopclaid = p2.oid)
              AND EXISTS (SELECT * FROM pg_amop AS p3
                          WHERE p3.amopid = p1.oid AND p3.amopclaid = p2.oid);

-- Check that amopopr points at a reasonable-looking operator, ie a binary
-- operator yielding boolean.
-- NOTE: for 7.1, add restriction that operator inputs are of same type.
-- We used to have opclasses like "int24_ops" but these were broken.

SELECT p1.oid, p2.oid, p2.oprname
FROM pg_amop AS p1, pg_operator AS p2
WHERE p1.amopopr = p2.oid AND
    (p2.oprkind != 'b' OR p2.oprresult != 16 OR p2.oprleft != p2.oprright);

-- If opclass is for a specific type, operator inputs should be of that type

SELECT p1.oid, p2.oid, p2.oprname, p3.oid, p3.opcname
FROM pg_amop AS p1, pg_operator AS p2, pg_opclass AS p3
WHERE p1.amopopr = p2.oid AND p1.amopclaid = p3.oid AND
    p3.opcdeftype != 0 AND
    (p3.opcdeftype != p2.oprleft OR p3.opcdeftype != p2.oprright);

-- **************** pg_amproc ****************

-- Look for illegal values in pg_amproc fields

SELECT p1.oid
FROM pg_amproc as p1
WHERE p1.amid = 0 OR p1.amopclaid = 0 OR p1.amproc = 0 OR
    p1.amprocnum <= 0;

-- Look for duplicate pg_amproc entries

SELECT p1.oid, p2.oid
FROM pg_amproc AS p1, pg_amproc AS p2
WHERE p1.oid != p2.oid AND
    p1.amid = p2.amid AND
    p1.amopclaid = p2.amopclaid AND
    p1.amprocnum = p2.amprocnum;

-- Cross-check amprocnum index against parent AM

SELECT p1.oid, p2.oid, p2.amname
FROM pg_amproc AS p1, pg_am AS p2
WHERE p1.amid = p2.oid AND p1.amprocnum > p2.amsupport;

-- Detect missing pg_amproc entries: should have as many support functions
-- as AM expects for each opclass, unless there are none at all
-- (some opclasses only offer support for a limited set of AMs...)

SELECT p1.oid, p1.amname, p2.oid, p2.opcname
FROM pg_am AS p1, pg_opclass AS p2
WHERE p1.amsupport != (SELECT count(*) FROM pg_amproc AS p3
                       WHERE p3.amid = p1.oid AND p3.amopclaid = p2.oid)
           AND EXISTS (SELECT * FROM pg_amproc AS p3
                       WHERE p3.amid = p1.oid AND p3.amopclaid = p2.oid);

-- Unfortunately, we can't check the amproc link very well because the
-- signature of the function may be different for different support routines
-- or different base data types.
-- We can check that all the referenced instances of the same support
-- routine number take the same number of parameters, but that's about it...

SELECT p1.oid, p2.oid, p2.proname, p3.oid, p4.oid, p4.proname
FROM pg_amproc AS p1, pg_proc AS p2, pg_amproc AS p3, pg_proc AS p4
WHERE p1.oid != p3.oid AND
    p1.amid = p3.amid AND p1.amprocnum = p3.amprocnum AND
    p1.amproc = p2.oid AND p3.amproc = p4.oid AND
    (p2.proretset OR p4.proretset OR p2.pronargs != p4.pronargs);

-- Cross-check that each opclass that has any entries for a given AM
-- has all the entries that any other opclass does.  This catches cases
-- where an opclass has pg_amop but not pg_amproc entries or vice versa.
-- (The above tests for missing pg_amop or pg_amproc entries are redundant
-- with this, but I'll leave them in place anyway.)

-- All the strategy index numbers used for each AM
CREATE TEMP TABLE amopstrategies AS
    SELECT DISTINCT amopid, amopstrategy FROM pg_amop;

-- All the support proc numbers used for each AM
CREATE TEMP TABLE amprocnums AS
    SELECT DISTINCT amid, amprocnum FROM pg_amproc;

-- All the opclasses that claim to have support for each AM in either table.
-- UNION implies DISTINCT, so we do not need DISTINCT in the sub-selects.
CREATE TEMP TABLE amopclassids AS
    SELECT amid, amopclaid FROM pg_amproc UNION
    SELECT amopid, amopclaid FROM pg_amop;

-- Look for AMs that are missing one or more strategy operators
SELECT * FROM amopclassids c, amopstrategies s
WHERE c.amid = s.amopid AND NOT EXISTS
      (SELECT 1 FROM pg_amop a WHERE a.amopid = c.amid AND
       a.amopclaid = c.amopclaid AND a.amopstrategy = s.amopstrategy);

-- Look for AMs that are missing one or more support procs
SELECT * FROM amopclassids c, amprocnums p
WHERE c.amid = p.amid AND NOT EXISTS
      (SELECT 1 FROM pg_amproc a WHERE a.amid = c.amid AND
       a.amopclaid = c.amopclaid AND a.amprocnum = p.amprocnum);
