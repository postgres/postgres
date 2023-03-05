/* contrib/pageinspect/pageinspect--1.10--1.11.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pageinspect UPDATE TO '1.11'" to load this file. \quit

--
-- Functions that fetch relation pages must be PARALLEL RESTRICTED,
-- not PARALLEL SAFE, otherwise they will fail when run on a
-- temporary table in a parallel worker process.
--

ALTER FUNCTION get_raw_page(text, int8) PARALLEL RESTRICTED;
ALTER FUNCTION get_raw_page(text, text, int8) PARALLEL RESTRICTED;
-- tuple_data_split must be restricted because it may fetch TOAST data.
ALTER FUNCTION tuple_data_split(oid, bytea, integer, integer, text) PARALLEL RESTRICTED;
ALTER FUNCTION tuple_data_split(oid, bytea, integer, integer, text, bool) PARALLEL RESTRICTED;
-- heap_page_item_attrs must be restricted because it calls tuple_data_split.
ALTER FUNCTION heap_page_item_attrs(bytea, regclass, bool) PARALLEL RESTRICTED;
ALTER FUNCTION heap_page_item_attrs(bytea, regclass) PARALLEL RESTRICTED;
ALTER FUNCTION bt_metap(text) PARALLEL RESTRICTED;
ALTER FUNCTION bt_page_stats(text, int8) PARALLEL RESTRICTED;
ALTER FUNCTION bt_page_items(text, int8) PARALLEL RESTRICTED;
ALTER FUNCTION hash_bitmap_info(regclass, int8) PARALLEL RESTRICTED;
-- brin_page_items might be parallel safe, because it seems to touch
-- only index metadata, but I don't think there's a point in risking it.
-- Likewise for gist_page_items.
ALTER FUNCTION brin_page_items(bytea, regclass) PARALLEL RESTRICTED;
ALTER FUNCTION gist_page_items(bytea, regclass) PARALLEL RESTRICTED;
