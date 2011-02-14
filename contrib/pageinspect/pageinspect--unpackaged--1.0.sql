/* contrib/pageinspect/pageinspect--unpackaged--1.0.sql */

ALTER EXTENSION pageinspect ADD function get_raw_page(text,integer);
ALTER EXTENSION pageinspect ADD function get_raw_page(text,text,integer);
ALTER EXTENSION pageinspect ADD function page_header(bytea);
ALTER EXTENSION pageinspect ADD function heap_page_items(bytea);
ALTER EXTENSION pageinspect ADD function bt_metap(text);
ALTER EXTENSION pageinspect ADD function bt_page_stats(text,integer);
ALTER EXTENSION pageinspect ADD function bt_page_items(text,integer);
ALTER EXTENSION pageinspect ADD function fsm_page_contents(bytea);
