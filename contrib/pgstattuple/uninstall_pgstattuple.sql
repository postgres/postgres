-- Adjust this setting to control where the objects get created.
SET search_path = public;

DROP FUNCTION pgstattuple(oid);
DROP FUNCTION pgstattuple(text);
DROP TYPE pgstattuple_type;

DROP FUNCTION pgstatindex(text);
DROP TYPE pgstatindex_type;

DROP FUNCTION bt_metap(text);
DROP TYPE bt_metap_type;

DROP FUNCTION bt_page_stats(text, int4);
DROP TYPE bt_page_stats_type;

DROP FUNCTION bt_page_items(text, int4);
DROP TYPE bt_page_items_type;

DROP FUNCTION pg_relpages(text);
