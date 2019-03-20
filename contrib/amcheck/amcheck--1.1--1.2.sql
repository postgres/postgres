/* contrib/amcheck/amcheck--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION amcheck UPDATE TO '1.2'" to load this file. \quit

-- In order to avoid issues with dependencies when updating amcheck to 1.2,
-- create new, overloaded version of the 1.1 function signature

--
-- bt_index_parent_check()
--
CREATE FUNCTION bt_index_parent_check(index regclass,
    heapallindexed boolean, rootdescend boolean)
RETURNS VOID
AS 'MODULE_PATHNAME', 'bt_index_parent_check'
LANGUAGE C STRICT PARALLEL RESTRICTED;

-- Don't want this to be available to public
REVOKE ALL ON FUNCTION bt_index_parent_check(regclass, boolean, boolean) FROM PUBLIC;
