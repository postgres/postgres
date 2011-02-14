/* contrib/pgstattuple/pgstattuple--unpackaged--1.0.sql */

ALTER EXTENSION pgstattuple ADD function pgstattuple(text);
ALTER EXTENSION pgstattuple ADD function pgstattuple(oid);
ALTER EXTENSION pgstattuple ADD function pgstatindex(text);
ALTER EXTENSION pgstattuple ADD function pg_relpages(text);
