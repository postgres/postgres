/* contrib/pageinspect/pageinspect--1.9--1.10.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pageinspect UPDATE TO '1.10'" to load this file. \quit

--
-- page_header()
--
DROP FUNCTION page_header(IN page bytea);
CREATE FUNCTION page_header(IN page bytea,
    OUT lsn pg_lsn,
    OUT checksum smallint,
    OUT flags smallint,
    OUT lower int,
    OUT upper int,
    OUT special int,
    OUT pagesize int,
    OUT version smallint,
    OUT prune_xid xid)
AS 'MODULE_PATHNAME', 'page_header'
LANGUAGE C STRICT PARALLEL SAFE;
