--
-- MISC_SANITY
-- Sanity checks for common errors in making system tables that don't fit
-- comfortably into either opr_sanity or type_sanity.
--
-- Every test failure in this file should be closely inspected.
-- The description of the failing test should be read carefully before
-- adjusting the expected output.  In most cases, the queries should
-- not find *any* matching entries.
--
-- NB: run this test early, because some later tests create bogus entries.


-- **************** pg_depend ****************

-- Look for illegal values in pg_depend fields.
-- classid/objid can be zero, but only in 'p' entries

SELECT *
FROM pg_depend as d1
WHERE refclassid = 0 OR refobjid = 0 OR
      deptype NOT IN ('a', 'e', 'i', 'n', 'p') OR
      (deptype != 'p' AND (classid = 0 OR objid = 0)) OR
      (deptype = 'p' AND (classid != 0 OR objid != 0 OR objsubid != 0));

-- **************** pg_shdepend ****************

-- Look for illegal values in pg_shdepend fields.
-- classid/objid can be zero, but only in 'p' entries

SELECT *
FROM pg_shdepend as d1
WHERE refclassid = 0 OR refobjid = 0 OR
      deptype NOT IN ('a', 'o', 'p', 'r') OR
      (deptype != 'p' AND (classid = 0 OR objid = 0)) OR
      (deptype = 'p' AND (dbid != 0 OR classid != 0 OR objid != 0 OR objsubid != 0));


-- Check each OID-containing system catalog to see if its lowest-numbered OID
-- is pinned.  If not, and if that OID was generated during initdb, then
-- perhaps initdb forgot to scan that catalog for pinnable entries.
-- Generally, it's okay for a catalog to be listed in the output of this
-- test if that catalog is scanned by initdb.c's setup_depend() function;
-- whatever OID the test is complaining about must have been added later
-- in initdb, where it intentionally isn't pinned.  Legitimate exceptions
-- to that rule are listed in the comments in setup_depend().

do $$
declare relnm text;
  reloid oid;
  shared bool;
  lowoid oid;
  pinned bool;
begin
for relnm, reloid, shared in
  select relname, oid, relisshared from pg_class
  where EXISTS(
      SELECT * FROM pg_attribute
      WHERE attrelid = pg_class.oid AND attname = 'oid')
    and relkind = 'r' and oid < 16384 order by 1
loop
  execute 'select min(oid) from ' || relnm into lowoid;
  continue when lowoid is null or lowoid >= 16384;
  if shared then
    pinned := exists(select 1 from pg_shdepend
                     where refclassid = reloid and refobjid = lowoid
                     and deptype = 'p');
  else
    pinned := exists(select 1 from pg_depend
                     where refclassid = reloid and refobjid = lowoid
                     and deptype = 'p');
  end if;
  if not pinned then
    raise notice '% contains unpinned initdb-created object(s)', relnm;
  end if;
end loop;
end$$;

-- **************** pg_class ****************

-- Look for system tables with varlena columns but no toast table. All
-- system tables with toastable columns should have toast tables, with
-- the following exceptions:
-- 1. pg_class, pg_attribute, and pg_index, due to fear of recursive
-- dependencies as toast tables depend on them.
-- 2. pg_largeobject and pg_largeobject_metadata.  Large object catalogs
-- and toast tables are mutually exclusive and large object data is handled
-- as user data by pg_upgrade, which would cause failures.

SELECT relname, attname, atttypid::regtype
FROM pg_class c JOIN pg_attribute a ON c.oid = attrelid
WHERE c.oid < 16384 AND
      reltoastrelid = 0 AND
      relkind = 'r' AND
      attstorage != 'p'
ORDER BY 1, 2;
