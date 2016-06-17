/* contrib/pg_visibility/pg_visibility--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_visibility UPDATE TO '1.1'" to load this file. \quit

CREATE FUNCTION pg_check_frozen(regclass, t_ctid OUT tid)
RETURNS SETOF tid
AS 'MODULE_PATHNAME', 'pg_check_frozen'
LANGUAGE C STRICT;

CREATE FUNCTION pg_check_visible(regclass, t_ctid OUT tid)
RETURNS SETOF tid
AS 'MODULE_PATHNAME', 'pg_check_visible'
LANGUAGE C STRICT;

CREATE FUNCTION pg_truncate_visibility_map(regclass)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_truncate_visibility_map'
LANGUAGE C STRICT
PARALLEL UNSAFE;  -- let's not make this any more dangerous

REVOKE ALL ON FUNCTION pg_check_frozen(regclass) FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_check_visible(regclass) FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_truncate_visibility_map(regclass) FROM PUBLIC;
