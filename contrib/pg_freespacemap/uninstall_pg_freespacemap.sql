-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP VIEW pg_freespacemap_pages;
DROP VIEW pg_freespacemap_relations;

DROP FUNCTION pg_freespacemap_pages();
DROP FUNCTION pg_freespacemap_relations();
