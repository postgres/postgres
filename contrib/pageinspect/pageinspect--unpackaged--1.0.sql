/* contrib/pageinspect/pageinspect--unpackaged--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pageinspect" to load this file. \quit

DROP FUNCTION heap_page_items(bytea);
CREATE FUNCTION heap_page_items(IN page bytea,
	OUT lp smallint,
	OUT lp_off smallint,
	OUT lp_flags smallint,
	OUT lp_len smallint,
	OUT t_xmin xid,
	OUT t_xmax xid,
	OUT t_field3 int4,
	OUT t_ctid tid,
	OUT t_infomask2 integer,
	OUT t_infomask integer,
	OUT t_hoff smallint,
	OUT t_bits text,
	OUT t_oid oid)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'heap_page_items'
LANGUAGE C STRICT;

ALTER EXTENSION pageinspect ADD function get_raw_page(text,integer);
ALTER EXTENSION pageinspect ADD function get_raw_page(text,text,integer);
ALTER EXTENSION pageinspect ADD function page_header(bytea);
ALTER EXTENSION pageinspect ADD function bt_metap(text);
ALTER EXTENSION pageinspect ADD function bt_page_stats(text,integer);
ALTER EXTENSION pageinspect ADD function bt_page_items(text,integer);
ALTER EXTENSION pageinspect ADD function fsm_page_contents(bytea);
