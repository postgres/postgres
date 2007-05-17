-- Adjust this setting to control where the objects get created.
SET search_path = public;

DROP FUNCTION get_raw_page(text, int4);

DROP FUNCTION page_header(bytea);
DROP TYPE page_header_type;

DROP FUNCTION heap_page_items(bytea);
DROP TYPE heap_page_items_type;

DROP FUNCTION bt_metap(text);
DROP TYPE bt_metap_type;

DROP FUNCTION bt_page_stats(text, int4);
DROP TYPE bt_page_stats_type;

DROP FUNCTION bt_page_items(text, int4);
DROP TYPE bt_page_items_type;

