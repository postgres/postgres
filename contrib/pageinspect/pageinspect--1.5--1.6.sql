/* contrib/pageinspect/pageinspect--1.5--1.6.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pageinspect UPDATE TO '1.6'" to load this file. \quit

--
-- HASH functions
--

--
-- hash_page_type()
--
CREATE FUNCTION hash_page_type(IN page bytea)
RETURNS text
AS 'MODULE_PATHNAME', 'hash_page_type'
LANGUAGE C STRICT PARALLEL SAFE;

--
-- hash_page_stats()
--
CREATE FUNCTION hash_page_stats(IN page bytea,
    OUT live_items int4,
    OUT dead_items int4,
    OUT page_size int4,
    OUT free_size int4,
    OUT hasho_prevblkno int8,
    OUT hasho_nextblkno int8,
    OUT hasho_bucket int8,
	OUT hasho_flag int4,
	OUT hasho_page_id int4)
AS 'MODULE_PATHNAME', 'hash_page_stats'
LANGUAGE C STRICT PARALLEL SAFE;

--
-- hash_page_items()
--
CREATE FUNCTION hash_page_items(IN page bytea,
	OUT itemoffset int4,
	OUT ctid tid,
	OUT data int8)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'hash_page_items'
LANGUAGE C STRICT PARALLEL SAFE;

--
-- hash_bitmap_info()
--
CREATE FUNCTION hash_bitmap_info(IN index_oid regclass, IN blkno int8,
	OUT bitmapblkno int8,
	OUT bitmapbit int4,
	OUT bitstatus bool)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'hash_bitmap_info'
LANGUAGE C STRICT PARALLEL SAFE;

--
-- hash_metapage_info()
--
CREATE FUNCTION hash_metapage_info(IN page bytea,
    OUT magic int8,
    OUT version int8,
    OUT ntuples double precision,
    OUT ffactor int4,
    OUT bsize int4,
    OUT bmsize int4,
    OUT bmshift int4,
    OUT maxbucket int8,
    OUT highmask int8,
    OUT lowmask int8,
    OUT ovflpoint int8,
    OUT firstfree int8,
    OUT nmaps int8,
    OUT procid oid,
    OUT spares int8[],
    OUT mapp int8[])
AS 'MODULE_PATHNAME', 'hash_metapage_info'
LANGUAGE C STRICT PARALLEL SAFE;

--
-- page_checksum()
--
CREATE FUNCTION page_checksum(IN page bytea, IN blkno int4)
RETURNS smallint
AS 'MODULE_PATHNAME', 'page_checksum'
LANGUAGE C STRICT PARALLEL SAFE;

--
-- bt_page_items_bytea()
--
CREATE FUNCTION bt_page_items(IN page bytea,
    OUT itemoffset smallint,
    OUT ctid tid,
    OUT itemlen smallint,
    OUT nulls bool,
    OUT vars bool,
    OUT data text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'bt_page_items_bytea'
LANGUAGE C STRICT PARALLEL SAFE;
