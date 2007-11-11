-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP FUNCTION pgstattuple(text);
DROP FUNCTION pgstattuple(oid);
DROP FUNCTION pgstatindex(text);
DROP FUNCTION pg_relpages(text);
