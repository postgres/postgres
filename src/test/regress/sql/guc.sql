-- pg_regress should ensure that this default value applies; however
-- we can't rely on any specific default value of vacuum_cost_delay
SHOW datestyle;

-- SET to some nondefault value
SET vacuum_cost_delay TO 40;
SET datestyle = 'ISO, YMD';
SHOW vacuum_cost_delay;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;

-- SET LOCAL has no effect outside of a transaction
SET LOCAL vacuum_cost_delay TO 50;
SHOW vacuum_cost_delay;
SET LOCAL datestyle = 'SQL';
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;

-- SET LOCAL within a transaction that commits
BEGIN;
SET LOCAL vacuum_cost_delay TO 50;
SHOW vacuum_cost_delay;
SET LOCAL datestyle = 'SQL';
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
COMMIT;
SHOW vacuum_cost_delay;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;

-- SET should be reverted after ROLLBACK
BEGIN;
SET vacuum_cost_delay TO 60;
SHOW vacuum_cost_delay;
SET datestyle = 'German';
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
ROLLBACK;
SHOW vacuum_cost_delay;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;

-- Some tests with subtransactions
BEGIN;
SET vacuum_cost_delay TO 70;
SET datestyle = 'MDY';
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
SAVEPOINT first_sp;
SET vacuum_cost_delay TO 80.1;
SHOW vacuum_cost_delay;
SET datestyle = 'German, DMY';
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
ROLLBACK TO first_sp;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
SAVEPOINT second_sp;
SET vacuum_cost_delay TO '900us';
SET datestyle = 'SQL, YMD';
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
SAVEPOINT third_sp;
SET vacuum_cost_delay TO 100;
SHOW vacuum_cost_delay;
SET datestyle = 'Postgres, MDY';
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
ROLLBACK TO third_sp;
SHOW vacuum_cost_delay;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
ROLLBACK TO second_sp;
SHOW vacuum_cost_delay;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
ROLLBACK;
SHOW vacuum_cost_delay;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;

-- SET LOCAL with Savepoints
BEGIN;
SHOW vacuum_cost_delay;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
SAVEPOINT sp;
SET LOCAL vacuum_cost_delay TO 30;
SHOW vacuum_cost_delay;
SET LOCAL datestyle = 'Postgres, MDY';
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
ROLLBACK TO sp;
SHOW vacuum_cost_delay;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
ROLLBACK;
SHOW vacuum_cost_delay;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;

-- SET LOCAL persists through RELEASE (which was not true in 8.0-8.2)
BEGIN;
SHOW vacuum_cost_delay;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
SAVEPOINT sp;
SET LOCAL vacuum_cost_delay TO 30;
SHOW vacuum_cost_delay;
SET LOCAL datestyle = 'Postgres, MDY';
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
RELEASE SAVEPOINT sp;
SHOW vacuum_cost_delay;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
ROLLBACK;
SHOW vacuum_cost_delay;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;

-- SET followed by SET LOCAL
BEGIN;
SET vacuum_cost_delay TO 40;
SET LOCAL vacuum_cost_delay TO 50;
SHOW vacuum_cost_delay;
SET datestyle = 'ISO, DMY';
SET LOCAL datestyle = 'Postgres, MDY';
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
COMMIT;
SHOW vacuum_cost_delay;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;

--
-- Test RESET.  We use datestyle because the reset value is forced by
-- pg_regress, so it doesn't depend on the installation's configuration.
--
SET datestyle = iso, ymd;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
RESET datestyle;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;

-- Test some simple error cases
SET seq_page_cost TO 'NaN';
SET vacuum_cost_delay TO '10s';
SET no_such_variable TO 42;

-- Test "custom" GUCs created on the fly (which aren't really an
-- intended feature, but many people use them).
SHOW custom.my_guc;  -- error, not known yet
SET custom.my_guc = 42;
SHOW custom.my_guc;
RESET custom.my_guc;  -- this makes it go to empty, not become unknown again
SHOW custom.my_guc;
SET custom.my.qualified.guc = 'foo';
SHOW custom.my.qualified.guc;
SET custom."bad-guc" = 42;  -- disallowed because -c cannot set this name
SHOW custom."bad-guc";
SET special."weird name" = 'foo';  -- could be allowed, but we choose not to
SHOW special."weird name";

--
-- Test DISCARD TEMP
--
CREATE TEMP TABLE reset_test ( data text ) ON COMMIT DELETE ROWS;
SELECT relname FROM pg_class WHERE relname = 'reset_test';
DISCARD TEMP;
SELECT relname FROM pg_class WHERE relname = 'reset_test';

--
-- Test DISCARD ALL
--

-- do changes
DECLARE foo CURSOR WITH HOLD FOR SELECT 1;
PREPARE foo AS SELECT 1;
LISTEN foo_event;
SET vacuum_cost_delay = 13;
CREATE TEMP TABLE tmp_foo (data text) ON COMMIT DELETE ROWS;
CREATE ROLE regress_guc_user;
SET SESSION AUTHORIZATION regress_guc_user;
-- look changes
SELECT pg_listening_channels();
SELECT name FROM pg_prepared_statements;
SELECT name FROM pg_cursors;
SHOW vacuum_cost_delay;
SELECT relname from pg_class where relname = 'tmp_foo';
SELECT current_user = 'regress_guc_user';
-- discard everything
DISCARD ALL;
-- look again
SELECT pg_listening_channels();
SELECT name FROM pg_prepared_statements;
SELECT name FROM pg_cursors;
SHOW vacuum_cost_delay;
SELECT relname from pg_class where relname = 'tmp_foo';
SELECT current_user = 'regress_guc_user';
DROP ROLE regress_guc_user;

--
-- search_path should react to changes in pg_namespace
--

set search_path = foo, public, not_there_initially;
select current_schemas(false);
create schema not_there_initially;
select current_schemas(false);
drop schema not_there_initially;
select current_schemas(false);
reset search_path;

--
-- Tests for function-local GUC settings
--

set work_mem = '3MB';

create function report_guc(text) returns text as
$$ select current_setting($1) $$ language sql
set work_mem = '1MB';

select report_guc('work_mem'), current_setting('work_mem');

alter function report_guc(text) set work_mem = '2MB';

select report_guc('work_mem'), current_setting('work_mem');

alter function report_guc(text) reset all;

select report_guc('work_mem'), current_setting('work_mem');

-- SET LOCAL is restricted by a function SET option
create or replace function myfunc(int) returns text as $$
begin
  set local work_mem = '2MB';
  return current_setting('work_mem');
end $$
language plpgsql
set work_mem = '1MB';

select myfunc(0), current_setting('work_mem');

alter function myfunc(int) reset all;

select myfunc(0), current_setting('work_mem');

set work_mem = '3MB';

-- but SET isn't
create or replace function myfunc(int) returns text as $$
begin
  set work_mem = '2MB';
  return current_setting('work_mem');
end $$
language plpgsql
set work_mem = '1MB';

select myfunc(0), current_setting('work_mem');

set work_mem = '3MB';

-- it should roll back on error, though
create or replace function myfunc(int) returns text as $$
begin
  set work_mem = '2MB';
  perform 1/$1;
  return current_setting('work_mem');
end $$
language plpgsql
set work_mem = '1MB';

select myfunc(0);
select current_setting('work_mem');
select myfunc(1), current_setting('work_mem');

-- check current_setting()'s behavior with invalid setting name

select current_setting('nosuch.setting');  -- FAIL
select current_setting('nosuch.setting', false);  -- FAIL
select current_setting('nosuch.setting', true) is null;

-- after this, all three cases should yield 'nada'
set nosuch.setting = 'nada';

select current_setting('nosuch.setting');
select current_setting('nosuch.setting', false);
select current_setting('nosuch.setting', true);

-- Normally, CREATE FUNCTION should complain about invalid values in
-- function SET options; but not if check_function_bodies is off,
-- because that creates ordering hazards for pg_dump

create function func_with_bad_set() returns int as $$ select 1 $$
language sql
set default_text_search_config = no_such_config;

set check_function_bodies = off;

create function func_with_bad_set() returns int as $$ select 1 $$
language sql
set default_text_search_config = no_such_config;

select func_with_bad_set();

reset check_function_bodies;

set default_with_oids to f;
-- Should not allow to set it to true.
set default_with_oids to t;

-- test SET unrecognized parameter
SET foo = false;  -- no such setting

-- test setting a parameter with a registered prefix (plpgsql)
SET plpgsql.extra_foo_warnings = false;  -- no such setting
SHOW plpgsql.extra_foo_warnings;  -- but the parameter is set

-- cleanup
RESET foo;
RESET plpgsql.extra_foo_warnings;
