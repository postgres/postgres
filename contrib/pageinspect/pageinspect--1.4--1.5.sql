/* contrib/pageinspect/pageinspect--1.4--1.5.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pageinspect UPDATE TO '1.5'" to load this file. \quit

ALTER FUNCTION get_raw_page(text, int4) PARALLEL SAFE;
ALTER FUNCTION get_raw_page(text, text, int4) PARALLEL SAFE;
ALTER FUNCTION page_header(bytea) PARALLEL SAFE;
ALTER FUNCTION heap_page_items(bytea) PARALLEL SAFE;
ALTER FUNCTION tuple_data_split(oid, bytea, integer, integer, text) PARALLEL SAFE;
ALTER FUNCTION tuple_data_split(oid, bytea, integer, integer, text, bool) PARALLEL SAFE;
ALTER FUNCTION heap_page_item_attrs(bytea, regclass, bool) PARALLEL SAFE;
ALTER FUNCTION heap_page_item_attrs(bytea, regclass) PARALLEL SAFE;
ALTER FUNCTION bt_metap(text) PARALLEL SAFE;
ALTER FUNCTION bt_page_stats(text, int4) PARALLEL SAFE;
ALTER FUNCTION bt_page_items(text, int4) PARALLEL SAFE;
ALTER FUNCTION brin_page_type(bytea) PARALLEL SAFE;
ALTER FUNCTION brin_metapage_info(bytea) PARALLEL SAFE;
ALTER FUNCTION brin_revmap_data(bytea) PARALLEL SAFE;
ALTER FUNCTION brin_page_items(bytea, regclass) PARALLEL SAFE;
ALTER FUNCTION fsm_page_contents(bytea) PARALLEL SAFE;
ALTER FUNCTION gin_metapage_info(bytea) PARALLEL SAFE;
ALTER FUNCTION gin_page_opaque_info(bytea) PARALLEL SAFE;
ALTER FUNCTION gin_leafpage_items(bytea) PARALLEL SAFE;
