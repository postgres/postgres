/* contrib/postgres_fdw/postgres_fdw--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION postgres_fdw UPDATE TO '1.2'" to load this file. \quit

/* First we have to remove it from the extension */
ALTER EXTENSION postgres_fdw DROP FUNCTION postgres_fdw_get_connections ();

/* Then we can drop it */
DROP FUNCTION postgres_fdw_get_connections ();

CREATE FUNCTION postgres_fdw_get_connections (
    IN check_conn boolean DEFAULT false, OUT server_name text,
    OUT user_name text, OUT valid boolean, OUT used_in_xact boolean,
    OUT closed boolean, OUT remote_backend_pid int4)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'postgres_fdw_get_connections_1_2'
LANGUAGE C STRICT PARALLEL RESTRICTED;
