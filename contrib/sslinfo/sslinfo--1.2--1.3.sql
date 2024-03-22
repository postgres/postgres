/* contrib/sslinfo/sslinfo--1.2--1.3.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION sslinfo" to load this file. \quit

CREATE FUNCTION ssl_client_get_notbefore() RETURNS timestamptz
AS 'MODULE_PATHNAME', 'ssl_client_get_notbefore'
LANGUAGE C STRICT PARALLEL RESTRICTED;

CREATE FUNCTION ssl_client_get_notafter() RETURNS timestamptz
AS 'MODULE_PATHNAME', 'ssl_client_get_notafter'
LANGUAGE C STRICT PARALLEL RESTRICTED;
