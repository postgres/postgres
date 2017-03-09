/* contrib/amcheck/amcheck--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION amcheck" to load this file. \quit

--
-- bt_index_check()
--
CREATE FUNCTION bt_index_check(index regclass)
RETURNS VOID
AS 'MODULE_PATHNAME', 'bt_index_check'
LANGUAGE C STRICT PARALLEL RESTRICTED;

--
-- bt_index_parent_check()
--
CREATE FUNCTION bt_index_parent_check(index regclass)
RETURNS VOID
AS 'MODULE_PATHNAME', 'bt_index_parent_check'
LANGUAGE C STRICT PARALLEL RESTRICTED;

-- Don't want these to be available to public
REVOKE ALL ON FUNCTION bt_index_check(regclass) FROM PUBLIC;
REVOKE ALL ON FUNCTION bt_index_parent_check(regclass) FROM PUBLIC;
