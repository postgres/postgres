/* contrib/pageinspect/pageinspect--1.7--1.8.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pageinspect UPDATE TO '1.8'" to load this file. \quit

--
-- heap_tuple_infomask_flags()
--
CREATE FUNCTION heap_tuple_infomask_flags(
       t_infomask integer,
       t_infomask2 integer,
       raw_flags OUT text[],
       combined_flags OUT text[])
RETURNS record
AS 'MODULE_PATHNAME', 'heap_tuple_infomask_flags'
LANGUAGE C STRICT PARALLEL SAFE;

--
-- bt_metap()
--
DROP FUNCTION bt_metap(text);
CREATE FUNCTION bt_metap(IN relname text,
    OUT magic int4,
    OUT version int4,
    OUT root int4,
    OUT level int4,
    OUT fastroot int4,
    OUT fastlevel int4,
    OUT oldest_xact int4,
    OUT last_cleanup_num_tuples real,
    OUT allequalimage boolean)
AS 'MODULE_PATHNAME', 'bt_metap'
LANGUAGE C STRICT PARALLEL SAFE;

--
-- bt_page_items(text, int4)
--
DROP FUNCTION bt_page_items(text, int4);
CREATE FUNCTION bt_page_items(IN relname text, IN blkno int4,
    OUT itemoffset smallint,
    OUT ctid tid,
    OUT itemlen smallint,
    OUT nulls bool,
    OUT vars bool,
    OUT data text,
    OUT dead boolean,
    OUT htid tid,
    OUT tids tid[])
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'bt_page_items'
LANGUAGE C STRICT PARALLEL SAFE;

--
-- bt_page_items(bytea)
--
DROP FUNCTION bt_page_items(bytea);
CREATE FUNCTION bt_page_items(IN page bytea,
    OUT itemoffset smallint,
    OUT ctid tid,
    OUT itemlen smallint,
    OUT nulls bool,
    OUT vars bool,
    OUT data text,
    OUT dead boolean,
    OUT htid tid,
    OUT tids tid[])
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'bt_page_items_bytea'
LANGUAGE C STRICT PARALLEL SAFE;
