/* contrib/pageinspect/pageinspect--1.8--1.9.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pageinspect UPDATE TO '1.9'" to load this file. \quit

--
-- gist_page_opaque_info()
--
CREATE FUNCTION gist_page_opaque_info(IN page bytea,
    OUT lsn pg_lsn,
    OUT nsn pg_lsn,
    OUT rightlink bigint,
    OUT flags text[])
AS 'MODULE_PATHNAME', 'gist_page_opaque_info'
LANGUAGE C STRICT PARALLEL SAFE;


--
-- gist_page_items_bytea()
--
CREATE FUNCTION gist_page_items_bytea(IN page bytea,
    OUT itemoffset smallint,
    OUT ctid tid,
    OUT itemlen smallint,
    OUT key_data bytea)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gist_page_items_bytea'
LANGUAGE C STRICT PARALLEL SAFE;

--
-- gist_page_items()
--
CREATE FUNCTION gist_page_items(IN page bytea,
    IN index_oid regclass,
    OUT itemoffset smallint,
    OUT ctid tid,
    OUT itemlen smallint,
    OUT keys text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gist_page_items'
LANGUAGE C STRICT PARALLEL SAFE;
