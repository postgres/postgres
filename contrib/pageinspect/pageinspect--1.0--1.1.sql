/* contrib/pageinspect/pageinspect--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pageinspect UPDATE TO '1.1'" to load this file. \quit

DROP FUNCTION page_header(bytea);
CREATE FUNCTION page_header(IN page bytea,
    OUT lsn text,
    OUT checksum smallint,
    OUT flags smallint,
    OUT lower smallint,
    OUT upper smallint,
    OUT special smallint,
    OUT pagesize smallint,
    OUT version smallint,
    OUT prune_xid xid)
AS 'MODULE_PATHNAME', 'page_header'
LANGUAGE C STRICT;
