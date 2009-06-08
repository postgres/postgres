/* $PostgreSQL: pgsql/contrib/pageinspect/uninstall_pageinspect.sql,v 1.5 2009/06/08 16:22:44 tgl Exp $ */

-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP FUNCTION get_raw_page(text, int4);
DROP FUNCTION get_raw_page(text, text, int4);
DROP FUNCTION page_header(bytea);
DROP FUNCTION heap_page_items(bytea);
DROP FUNCTION bt_metap(text);
DROP FUNCTION bt_page_stats(text, int4);
DROP FUNCTION bt_page_items(text, int4);
DROP FUNCTION fsm_page_contents(bytea);
