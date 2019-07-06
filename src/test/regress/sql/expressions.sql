--
-- expression evaluation tests that don't fit into a more specific file
--

--
-- Tests for SQLVAlueFunction
--


-- current_date  (always matches because of transactional behaviour)
SELECT date(now())::text = current_date::text;


-- current_time / localtime
SELECT now()::timetz::text = current_time::text;
SELECT now()::timetz(4)::text = current_time(4)::text;
SELECT now()::time::text = localtime::text;
SELECT now()::time(3)::text = localtime(3)::text;

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


--
-- Tests for BETWEEN
--

explain (costs off)
select count(*) from date_tbl
  where f1 between '1997-01-01' and '1998-01-01';
select count(*) from date_tbl
  where f1 between '1997-01-01' and '1998-01-01';

explain (costs off)
select count(*) from date_tbl
  where f1 not between '1997-01-01' and '1998-01-01';
select count(*) from date_tbl
  where f1 not between '1997-01-01' and '1998-01-01';

explain (costs off)
select count(*) from date_tbl
  where f1 between symmetric '1997-01-01' and '1998-01-01';
select count(*) from date_tbl
  where f1 between symmetric '1997-01-01' and '1998-01-01';

explain (costs off)
select count(*) from date_tbl
  where f1 not between symmetric '1997-01-01' and '1998-01-01';
select count(*) from date_tbl
  where f1 not between symmetric '1997-01-01' and '1998-01-01';
