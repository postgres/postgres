/* contrib/amcheck/amcheck--1.3--1.4.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION amcheck UPDATE TO '1.4'" to load this file. \quit

-- In order to avoid issues with dependencies when updating amcheck to 1.4,
-- create new, overloaded versions of the 1.2 bt_index_parent_check signature,
-- and 1.1 bt_index_check signature.

--
-- bt_index_parent_check()
--
CREATE FUNCTION bt_index_parent_check(index regclass,
    heapallindexed boolean, rootdescend boolean, checkunique boolean)
RETURNS VOID
AS 'MODULE_PATHNAME', 'bt_index_parent_check'
LANGUAGE C STRICT PARALLEL RESTRICTED;
--
-- bt_index_check()
--
CREATE FUNCTION bt_index_check(index regclass,
    heapallindexed boolean, checkunique boolean)
RETURNS VOID
AS 'MODULE_PATHNAME', 'bt_index_check'
LANGUAGE C STRICT PARALLEL RESTRICTED;

-- We don't want this to be available to public
REVOKE ALL ON FUNCTION bt_index_parent_check(regclass, boolean, boolean, boolean) FROM PUBLIC;
REVOKE ALL ON FUNCTION bt_index_check(regclass, boolean, boolean) FROM PUBLIC;
