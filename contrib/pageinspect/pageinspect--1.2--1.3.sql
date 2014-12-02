/* contrib/pageinspect/pageinspect--1.2--1.3.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pageinspect UPDATE TO '1.3'" to load this file. \quit

--
-- brin_page_type()
--
CREATE FUNCTION brin_page_type(IN page bytea)
RETURNS text
AS 'MODULE_PATHNAME', 'brin_page_type'
LANGUAGE C STRICT;

--
-- brin_metapage_info()
--
CREATE FUNCTION brin_metapage_info(IN page bytea, OUT magic text,
	OUT version integer, OUT pagesperrange integer, OUT lastrevmappage bigint)
AS 'MODULE_PATHNAME', 'brin_metapage_info'
LANGUAGE C STRICT;

--
-- brin_revmap_data()
--
CREATE FUNCTION brin_revmap_data(IN page bytea,
	OUT pages tid)
RETURNS SETOF tid
AS 'MODULE_PATHNAME', 'brin_revmap_data'
LANGUAGE C STRICT;

--
-- brin_page_items()
--
CREATE FUNCTION brin_page_items(IN page bytea, IN index_oid regclass,
	OUT itemoffset int,
	OUT blknum int,
	OUT attnum int,
	OUT allnulls bool,
	OUT hasnulls bool,
	OUT placeholder bool,
	OUT value text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'brin_page_items'
LANGUAGE C STRICT;

--
-- gin_metapage_info()
--
CREATE FUNCTION gin_metapage_info(IN page bytea,
    OUT pending_head bigint,
    OUT pending_tail bigint,
    OUT tail_free_size int4,
    OUT n_pending_pages bigint,
    OUT n_pending_tuples bigint,
    OUT n_total_pages bigint,
    OUT n_entry_pages bigint,
    OUT n_data_pages bigint,
    OUT n_entries bigint,
    OUT version int4)
AS 'MODULE_PATHNAME', 'gin_metapage_info'
LANGUAGE C STRICT;

--
-- gin_page_opaque_info()
--
CREATE FUNCTION gin_page_opaque_info(IN page bytea,
    OUT rightlink bigint,
    OUT maxoff int4,
    OUT flags text[])
AS 'MODULE_PATHNAME', 'gin_page_opaque_info'
LANGUAGE C STRICT;

--
-- gin_leafpage_items()
--
CREATE FUNCTION gin_leafpage_items(IN page bytea,
    OUT first_tid tid,
    OUT nbytes int2,
    OUT tids tid[])
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gin_leafpage_items'
LANGUAGE C STRICT;
