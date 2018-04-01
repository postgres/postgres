/* contrib/amcheck/amcheck--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION amcheck UPDATE TO '1.1'" to load this file. \quit

-- In order to avoid issues with dependencies when updating amcheck to 1.1,
-- create new, overloaded versions of the 1.0 functions

--
-- bt_index_check()
--
CREATE FUNCTION bt_index_check(index regclass,
    heapallindexed boolean)
RETURNS VOID
AS 'MODULE_PATHNAME', 'bt_index_check'
LANGUAGE C STRICT PARALLEL RESTRICTED;

--
-- bt_index_parent_check()
--
CREATE FUNCTION bt_index_parent_check(index regclass,
    heapallindexed boolean)
RETURNS VOID
AS 'MODULE_PATHNAME', 'bt_index_parent_check'
LANGUAGE C STRICT PARALLEL RESTRICTED;

-- Don't want these to be available to public
REVOKE ALL ON FUNCTION bt_index_check(regclass, boolean) FROM PUBLIC;
REVOKE ALL ON FUNCTION bt_index_parent_check(regclass, boolean) FROM PUBLIC;
