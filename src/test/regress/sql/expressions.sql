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

-- current_role/user/user is tested in rolenames.sql

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


--
-- Test parsing of a no-op cast to a type with unspecified typmod
--
begin;

create table numeric_tbl (f1 numeric(18,3), f2 numeric);

create view numeric_view as
  select
    f1, f1::numeric(16,4) as f1164, f1::numeric as f1n,
    f2, f2::numeric(16,4) as f2164, f2::numeric as f2n
  from numeric_tbl;

\d+ numeric_view

explain (verbose, costs off) select * from numeric_view;

-- bpchar, lacking planner support for its length coercion function,
-- could behave differently

create table bpchar_tbl (f1 character(16) unique, f2 bpchar);

create view bpchar_view as
  select
    f1, f1::character(14) as f114, f1::bpchar as f1n,
    f2, f2::character(14) as f214, f2::bpchar as f2n
  from bpchar_tbl;

\d+ bpchar_view

explain (verbose, costs off) select * from bpchar_view
  where f1::bpchar = 'foo';

rollback;


--
-- Ordinarily, IN/NOT IN can be converted to a ScalarArrayOpExpr
-- with a suitably-chosen array type.
--
explain (verbose, costs off)
select random() IN (1, 4, 8.0);
explain (verbose, costs off)
select random()::int IN (1, 4, 8.0);
-- However, if there's not a common supertype for the IN elements,
-- we should instead try to produce "x = v1 OR x = v2 OR ...".
-- In most cases that'll fail for lack of all the requisite = operators,
-- but it can succeed sometimes.  So this should complain about lack of
-- an = operator, not about cast failure.
select '(0,0)'::point in ('(0,0,0,0)'::box, point(0,0));
