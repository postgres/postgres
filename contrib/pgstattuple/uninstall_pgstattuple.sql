-- Adjust this setting to control where the objects get created.
SET search_path = public;

DROP FUNCTION pgstattuple(oid);
DROP FUNCTION pgstattuple(text);
DROP TYPE pgstattuple_type;

DROP FUNCTION pgstatindex(text);
DROP TYPE pgstatindex_type;

DROP FUNCTION pg_relpages(text);
