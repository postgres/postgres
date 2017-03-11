--
-- expression evaluated tests that don't fit into a more specific file
--

--
-- Tests for SQLVAlueFunction
--


-- current_date  (always matches because of transactional behaviour)
SELECT date(now())::text = current_date::text;


-- current_time / localtime
SELECT now()::timetz::text = current_time::text;
SELECT now()::time::text = localtime::text;

-- current_timestamp / localtimestamp (always matches because of transactional behaviour)
SELECT current_timestamp = NOW();
-- precision
SELECT length(current_timestamp::text) >= length(current_timestamp(0)::text);
-- localtimestamp
SELECT now()::timestamp::text = localtimestamp::text;

-- current_role/user/user is tested in rolnames.sql

-- current database / catalog
SELECT current_catalog = current_database();

-- current_schema
SELECT current_schema;
SET search_path = 'notme';
SELECT current_schema;
SET search_path = 'pg_catalog';
SELECT current_schema;
RESET search_path;
