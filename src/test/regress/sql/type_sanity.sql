--
-- TYPE_SANITY
-- Sanity checks for common errors in making type-related system tables:
-- pg_type, pg_class, pg_attribute.
--
-- None of the SELECTs here should ever find any matching entries,
-- so the expected output is easy to maintain ;-).
-- A test failure indicates someone messed up an entry in the system tables.
--
-- NB: we assume the oidjoins test will have caught any dangling links,
-- that is OID or REGPROC fields that are not zero and do not match some
-- row in the linked-to table.  However, if we want to enforce that a link
-- field can't be 0, we have to check it here.

-- **************** pg_type ****************

-- Look for illegal values in pg_type fields.

SELECT p1.oid, p1.typname
FROM pg_type as p1
WHERE (p1.typlen <= 0 AND p1.typlen != -1) OR
    (p1.typtype != 'b' AND p1.typtype != 'c') OR
    NOT p1.typisdefined OR
    (p1.typalign != 'c' AND p1.typalign != 's' AND
     p1.typalign != 'i' AND p1.typalign != 'd') OR
    (p1.typstorage != 'p' AND p1.typstorage != 'x' AND
     p1.typstorage != 'e' AND p1.typstorage != 'm');

-- Look for "pass by value" types that can't be passed by value.

SELECT p1.oid, p1.typname
FROM pg_type as p1
WHERE p1.typbyval AND
    (p1.typlen != 1 OR p1.typalign != 'c') AND
    (p1.typlen != 2 OR p1.typalign != 's') AND
    (p1.typlen != 4 OR p1.typalign != 'i');

-- Look for "toastable" types that aren't varlena.

SELECT p1.oid, p1.typname
FROM pg_type as p1
WHERE p1.typstorage != 'p' AND
    (p1.typbyval OR p1.typlen != -1);

-- Look for complex types that do not have a typrelid entry,
-- or basic types that do.

SELECT p1.oid, p1.typname
FROM pg_type as p1
WHERE (p1.typtype = 'c' AND p1.typrelid = 0) OR
    (p1.typtype != 'c' AND p1.typrelid != 0);

-- Look for basic types that don't have an array type.
-- NOTE: as of 7.3, this check finds SET, smgr, and unknown.

SELECT p1.oid, p1.typname
FROM pg_type as p1
WHERE p1.typtype != 'c' AND p1.typname NOT LIKE '\\_%' AND NOT EXISTS
    (SELECT 1 FROM pg_type as p2
     WHERE p2.typname = ('_' || p1.typname)::name AND
           p2.typelem = p1.oid);

-- Look for array types that don't have an equality operator.

SELECT p1.oid, p1.typname
FROM pg_type as p1
WHERE p1.typtype != 'c' AND p1.typname LIKE '\\_%' AND NOT EXISTS
    (SELECT 1 FROM pg_operator
     WHERE oprname = '=' AND oprleft = p1.oid AND oprright = p1.oid);

-- Conversion routines must be provided except in 'c' entries.

SELECT p1.oid, p1.typname
FROM pg_type as p1
WHERE p1.typtype != 'c' AND
    (p1.typinput = 0 OR p1.typoutput = 0 OR
     p1.typreceive = 0 OR p1.typsend = 0);

-- Check for bogus typinput routines
-- FIXME: ought to check prorettype, but there are special cases that make it
-- hard: prorettype might be binary-compatible with the type but not the same,
-- and for array types array_in's result has nothing to do with anything.

SELECT p1.oid, p1.typname, p2.oid, p2.proname
FROM pg_type AS p1, pg_proc AS p2
WHERE p1.typinput = p2.oid AND p1.typtype = 'b' AND
    (p2.pronargs != 1 OR p2.proretset) AND
    (p2.pronargs != 3 OR p2.proretset OR p2.proargtypes[2] != 'int4'::regtype);

-- Check for bogus typoutput routines
-- The first OR subclause detects bogus non-array cases,
-- the second one detects bogus array cases.
-- FIXME: ought to check prorettype, but not clear what it should be.

SELECT p1.oid, p1.typname, p2.oid, p2.proname
FROM pg_type AS p1, pg_proc AS p2
WHERE p1.typoutput = p2.oid AND p1.typtype = 'b' AND
    (p2.pronargs != 1 OR p2.proretset) AND
    (p2.pronargs != 2 OR p2.proretset OR p1.typelem = 0);

-- Check for bogus typreceive routines
-- FIXME: ought to check prorettype, but there are special cases that make it
-- hard: prorettype might be binary-compatible with the type but not the same,
-- and for array types array_in's result has nothing to do with anything.

SELECT p1.oid, p1.typname, p2.oid, p2.proname
FROM pg_type AS p1, pg_proc AS p2
WHERE p1.typreceive = p2.oid AND p1.typtype = 'b' AND
    (p2.pronargs != 1 OR p2.proretset) AND
    (p2.pronargs != 3 OR p2.proretset OR p2.proargtypes[2] != 'int4'::regtype);

-- Check for bogus typsend routines
-- The first OR subclause detects bogus non-array cases,
-- the second one detects bogus array cases.
-- FIXME: ought to check prorettype, but not clear what it should be.

SELECT p1.oid, p1.typname, p2.oid, p2.proname
FROM pg_type AS p1, pg_proc AS p2
WHERE p1.typsend = p2.oid AND p1.typtype = 'b' AND
    (p2.pronargs != 1 OR p2.proretset) AND
    (p2.pronargs != 2 OR p2.proretset OR p1.typelem = 0);

-- **************** pg_class ****************

-- Look for illegal values in pg_class fields

SELECT p1.oid, p1.relname
FROM pg_class as p1
WHERE p1.relkind NOT IN ('r', 'i', 's', 'S', 't', 'v');

-- Indexes should have an access method, others not.

SELECT p1.oid, p1.relname
FROM pg_class as p1
WHERE (p1.relkind = 'i' AND p1.relam = 0) OR
    (p1.relkind != 'i' AND p1.relam != 0);

-- **************** pg_attribute ****************

-- Look for illegal values in pg_attribute fields

SELECT p1.attrelid, p1.attname
FROM pg_attribute as p1
WHERE p1.attrelid = 0 OR p1.atttypid = 0 OR p1.attnum = 0 OR
    p1.attcacheoff != -1;

-- Cross-check attnum against parent relation

SELECT p1.attrelid, p1.attname, p2.oid, p2.relname
FROM pg_attribute AS p1, pg_class AS p2
WHERE p1.attrelid = p2.oid AND p1.attnum > p2.relnatts;

-- Detect missing pg_attribute entries: should have as many non-system
-- attributes as parent relation expects

SELECT p1.oid, p1.relname
FROM pg_class AS p1
WHERE p1.relnatts != (SELECT count(*) FROM pg_attribute AS p2
                      WHERE p2.attrelid = p1.oid AND p2.attnum > 0);

-- Cross-check against pg_type entry
-- NOTE: we allow attstorage to be 'plain' even when typstorage is not;
-- this is mainly for toast tables.

SELECT p1.attrelid, p1.attname, p2.oid, p2.typname
FROM pg_attribute AS p1, pg_type AS p2
WHERE p1.atttypid = p2.oid AND
    (p1.attlen != p2.typlen OR
     p1.attalign != p2.typalign OR
     p1.attbyval != p2.typbyval OR
     (p1.attstorage != p2.typstorage AND p1.attstorage != 'p'));
