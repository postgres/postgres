---------------------------------------------------------------------------
--
-- syscat.sql-
--    sample queries to the system catalogs
--
--
-- Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
-- Portions Copyright (c) 1994, Regents of the University of California
--
-- src/tutorial/syscat.source
--
---------------------------------------------------------------------------

--
-- Sets the schema search path to pg_catalog first, so that we do not
-- need to qualify every system object
--
SET search_path TO pg_catalog;

-- The LIKE pattern language requires underscores to be escaped, so make
-- sure the backslashes are not misinterpreted.
SET standard_conforming_strings TO on;

--
-- lists the names of all database owners and the name of their database(s)
--
SELECT rolname, datname
  FROM pg_roles, pg_database
  WHERE pg_roles.oid = datdba
  ORDER BY rolname, datname;

--
-- lists all user-defined classes
--
SELECT n.nspname, c.relname
  FROM pg_class c, pg_namespace n
  WHERE c.relnamespace=n.oid
    and c.relkind = 'r'                   -- not indices, views, etc
    and n.nspname not like 'pg\_%'       -- not catalogs
    and n.nspname != 'information_schema' -- not information_schema
  ORDER BY nspname, relname;


--
-- lists all simple indices (ie. those that are defined over one simple
-- column reference)
--
SELECT n.nspname AS schema_name,
       bc.relname AS class_name,
       ic.relname AS index_name,
       a.attname
  FROM pg_namespace n,
       pg_class bc,             -- base class
       pg_class ic,             -- index class
       pg_index i,
       pg_attribute a           -- att in base
  WHERE bc.relnamespace = n.oid
     and i.indrelid = bc.oid
     and i.indexrelid = ic.oid
     and i.indkey[0] = a.attnum
     and i.indnatts = 1
     and a.attrelid = bc.oid
  ORDER BY schema_name, class_name, index_name, attname;


--
-- lists the user-defined attributes and their types for all user-defined
-- classes
--
SELECT n.nspname, c.relname, a.attname, format_type(t.oid, null) as typname
  FROM pg_namespace n, pg_class c,
       pg_attribute a, pg_type t
  WHERE n.oid = c.relnamespace
    and c.relkind = 'r'     -- no indices
    and n.nspname not like 'pg\_%' -- no catalogs
    and n.nspname != 'information_schema' -- no information_schema
    and a.attnum > 0        -- no system att's
    and not a.attisdropped   -- no dropped columns
    and a.attrelid = c.oid
    and a.atttypid = t.oid
  ORDER BY nspname, relname, attname;


--
-- lists all user-defined base types (not including array types)
--
SELECT n.nspname, r.rolname, format_type(t.oid, null) as typname
  FROM pg_type t, pg_roles r, pg_namespace n
  WHERE r.oid = t.typowner
    and t.typnamespace = n.oid
    and t.typrelid = 0   -- no complex types
    and t.typelem = 0    -- no arrays
    and n.nspname not like 'pg\_%' -- no built-in types
    and n.nspname != 'information_schema' -- no information_schema
  ORDER BY nspname, rolname, typname;


--
-- lists all prefix operators
--
SELECT n.nspname, o.oprname AS prefix_op,
       format_type(right_type.oid, null) AS operand,
       format_type(result.oid, null) AS return_type
  FROM pg_namespace n, pg_operator o,
       pg_type right_type, pg_type result
  WHERE o.oprnamespace = n.oid
    and o.oprkind = 'l'           -- prefix ("left unary")
    and o.oprright = right_type.oid
    and o.oprresult = result.oid
  ORDER BY nspname, operand;


--
-- lists all infix operators
--
SELECT n.nspname, o.oprname AS binary_op,
       format_type(left_type.oid, null) AS left_opr,
       format_type(right_type.oid, null) AS right_opr,
       format_type(result.oid, null) AS return_type
  FROM pg_namespace n, pg_operator o, pg_type left_type,
       pg_type right_type, pg_type result
  WHERE o.oprnamespace = n.oid
    and o.oprkind = 'b'         -- infix ("binary")
    and o.oprleft = left_type.oid
    and o.oprright = right_type.oid
    and o.oprresult = result.oid
  ORDER BY nspname, left_opr, right_opr;


--
-- lists the name, number of arguments and the return type of all user-defined
-- C functions
--
SELECT n.nspname, p.proname, p.pronargs, format_type(t.oid, null) as return_type
  FROM pg_namespace n, pg_proc p,
       pg_language l, pg_type t
  WHERE p.pronamespace = n.oid
    and n.nspname not like 'pg\_%' -- no catalogs
    and n.nspname != 'information_schema' -- no information_schema
    and p.prolang = l.oid
    and p.prorettype = t.oid
    and l.lanname = 'c'
  ORDER BY nspname, proname, pronargs, return_type;

--
-- lists all aggregate functions and the types to which they can be applied
--
SELECT n.nspname, p.proname, format_type(t.oid, null) as typname
  FROM pg_namespace n, pg_aggregate a,
       pg_proc p, pg_type t
  WHERE p.pronamespace = n.oid
    and a.aggfnoid = p.oid
    and p.proargtypes[0] = t.oid
  ORDER BY nspname, proname, typname;


--
-- lists all the operator families that can be used with each access method
-- as well as the operators that can be used with the respective operator
-- families
--
SELECT am.amname, n.nspname, opf.opfname, opr.oprname
  FROM pg_namespace n, pg_am am, pg_opfamily opf,
       pg_amop amop, pg_operator opr
  WHERE opf.opfnamespace = n.oid
    and opf.opfmethod = am.oid
    and amop.amopfamily = opf.oid
    and amop.amopopr = opr.oid
  ORDER BY nspname, amname, opfname, oprname;

--
-- Reset the search path and standard_conforming_strings to their defaults
--
RESET search_path;
RESET standard_conforming_strings;
