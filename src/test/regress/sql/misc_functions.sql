-- directory paths and dlsuffix are passed to us in environment variables
\getenv libdir PG_LIBDIR
\getenv dlsuffix PG_DLSUFFIX

\set regresslib :libdir '/regress' :dlsuffix

-- Function to assist with verifying EXPLAIN which includes costs.  A series
-- of bool flags allows control over which portions are masked out
CREATE FUNCTION explain_mask_costs(query text, do_analyze bool,
    hide_costs bool, hide_row_est bool, hide_width bool) RETURNS setof text
LANGUAGE plpgsql AS
$$
DECLARE
    ln text;
    analyze_str text;
BEGIN
    IF do_analyze = true THEN
        analyze_str := 'on';
    ELSE
        analyze_str := 'off';
    END IF;

    -- avoid jit related output by disabling it
    SET LOCAL jit = 0;

    FOR ln IN
        EXECUTE format('explain (analyze %s, costs on, summary off, timing off, buffers off) %s',
            analyze_str, query)
    LOOP
        IF hide_costs = true THEN
            ln := regexp_replace(ln, 'cost=\d+\.\d\d\.\.\d+\.\d\d', 'cost=N..N');
        END IF;

        IF hide_row_est = true THEN
            -- don't use 'g' so that we leave the actual rows intact
            ln := regexp_replace(ln, 'rows=\d+', 'rows=N');
        END IF;

        IF hide_width = true THEN
            ln := regexp_replace(ln, 'width=\d+', 'width=N');
        END IF;

        RETURN NEXT ln;
    END LOOP;
END;
$$;

--
-- num_nulls()
--

SELECT num_nonnulls(NULL);
SELECT num_nonnulls('1');
SELECT num_nonnulls(NULL::text);
SELECT num_nonnulls(NULL::text, NULL::int);
SELECT num_nonnulls(1, 2, NULL::text, NULL::point, '', int8 '9', 1.0 / NULL);
SELECT num_nonnulls(VARIADIC '{1,2,NULL,3}'::int[]);
SELECT num_nonnulls(VARIADIC '{"1","2","3","4"}'::text[]);
SELECT num_nonnulls(VARIADIC ARRAY(SELECT CASE WHEN i <> 40 THEN i END FROM generate_series(1, 100) i));

SELECT num_nulls(NULL);
SELECT num_nulls('1');
SELECT num_nulls(NULL::text);
SELECT num_nulls(NULL::text, NULL::int);
SELECT num_nulls(1, 2, NULL::text, NULL::point, '', int8 '9', 1.0 / NULL);
SELECT num_nulls(VARIADIC '{1,2,NULL,3}'::int[]);
SELECT num_nulls(VARIADIC '{"1","2","3","4"}'::text[]);
SELECT num_nulls(VARIADIC ARRAY(SELECT CASE WHEN i <> 40 THEN i END FROM generate_series(1, 100) i));

-- special cases
SELECT num_nonnulls(VARIADIC NULL::text[]);
SELECT num_nonnulls(VARIADIC '{}'::int[]);
SELECT num_nulls(VARIADIC NULL::text[]);
SELECT num_nulls(VARIADIC '{}'::int[]);

-- should fail, one or more arguments is required
SELECT num_nonnulls();
SELECT num_nulls();

--
-- canonicalize_path()
--

CREATE FUNCTION test_canonicalize_path(text)
   RETURNS text
   AS :'regresslib'
   LANGUAGE C STRICT IMMUTABLE;

SELECT test_canonicalize_path('/');
SELECT test_canonicalize_path('/./abc/def/');
SELECT test_canonicalize_path('/./../abc/def');
SELECT test_canonicalize_path('/./../../abc/def/');
SELECT test_canonicalize_path('/abc/.././def/ghi');
SELECT test_canonicalize_path('/abc/./../def/ghi//');
SELECT test_canonicalize_path('/abc/def/../..');
SELECT test_canonicalize_path('/abc/def/../../..');
SELECT test_canonicalize_path('/abc/def/../../../../ghi/jkl');
SELECT test_canonicalize_path('.');
SELECT test_canonicalize_path('./');
SELECT test_canonicalize_path('./abc/..');
SELECT test_canonicalize_path('abc/../');
SELECT test_canonicalize_path('abc/../def');
SELECT test_canonicalize_path('..');
SELECT test_canonicalize_path('../abc/def');
SELECT test_canonicalize_path('../abc/..');
SELECT test_canonicalize_path('../abc/../def');
SELECT test_canonicalize_path('../abc/../../def/ghi');
SELECT test_canonicalize_path('./abc/./def/.');
SELECT test_canonicalize_path('./abc/././def/.');
SELECT test_canonicalize_path('./abc/./def/.././ghi/../../../jkl/mno');

--
-- pg_log_backend_memory_contexts()
--
-- Memory contexts are logged and they are not returned to the function.
-- Furthermore, their contents can vary depending on the timing. However,
-- we can at least verify that the code doesn't fail, and that the
-- permissions are set properly.
--

SELECT pg_log_backend_memory_contexts(pg_backend_pid());

SELECT pg_log_backend_memory_contexts(pid) FROM pg_stat_activity
  WHERE backend_type = 'checkpointer';

CREATE ROLE regress_log_memory;

SELECT has_function_privilege('regress_log_memory',
  'pg_log_backend_memory_contexts(integer)', 'EXECUTE'); -- no

GRANT EXECUTE ON FUNCTION pg_log_backend_memory_contexts(integer)
  TO regress_log_memory;

SELECT has_function_privilege('regress_log_memory',
  'pg_log_backend_memory_contexts(integer)', 'EXECUTE'); -- yes

SET ROLE regress_log_memory;
SELECT pg_log_backend_memory_contexts(pg_backend_pid());
RESET ROLE;

REVOKE EXECUTE ON FUNCTION pg_log_backend_memory_contexts(integer)
  FROM regress_log_memory;

DROP ROLE regress_log_memory;

--
-- Test some built-in SRFs
--
-- The outputs of these are variable, so we can't just print their results
-- directly, but we can at least verify that the code doesn't fail.
--
select setting as segsize
from pg_settings where name = 'wal_segment_size'
\gset

select count(*) > 0 as ok from pg_ls_waldir();
-- Test ProjectSet as well as FunctionScan
select count(*) > 0 as ok from (select pg_ls_waldir()) ss;
-- Test not-run-to-completion cases.
select * from pg_ls_waldir() limit 0;
select count(*) > 0 as ok from (select * from pg_ls_waldir() limit 1) ss;
select (w).size = :segsize as ok
from (select pg_ls_waldir() w) ss where length((w).name) = 24 limit 1;

select count(*) >= 0 as ok from pg_ls_archive_statusdir();
select count(*) >= 0 as ok from pg_ls_summariesdir();

-- pg_read_file()
select length(pg_read_file('postmaster.pid')) > 20;
select length(pg_read_file('postmaster.pid', 1, 20));
-- Test missing_ok
select pg_read_file('does not exist'); -- error
select pg_read_file('does not exist', true) IS NULL; -- ok
-- Test invalid argument
select pg_read_file('does not exist', 0, -1); -- error
select pg_read_file('does not exist', 0, -1, true); -- error

-- pg_read_binary_file()
select length(pg_read_binary_file('postmaster.pid')) > 20;
select length(pg_read_binary_file('postmaster.pid', 1, 20));
-- Test missing_ok
select pg_read_binary_file('does not exist'); -- error
select pg_read_binary_file('does not exist', true) IS NULL; -- ok
-- Test invalid argument
select pg_read_binary_file('does not exist', 0, -1); -- error
select pg_read_binary_file('does not exist', 0, -1, true); -- error

-- pg_stat_file()
select size > 20, isdir from pg_stat_file('postmaster.pid');

-- pg_ls_dir()
select * from (select pg_ls_dir('.') a) a where a = 'base' limit 1;
-- Test missing_ok (second argument)
select pg_ls_dir('does not exist', false, false); -- error
select pg_ls_dir('does not exist', true, false); -- ok
-- Test include_dot_dirs (third argument)
select count(*) = 1 as dot_found
  from pg_ls_dir('.', false, true) as ls where ls = '.';
select count(*) = 1 as dot_found
  from pg_ls_dir('.', false, false) as ls where ls = '.';

-- pg_timezone_names()
select * from (select (pg_timezone_names()).name) ptn where name='UTC' limit 1;

-- pg_tablespace_databases()
select count(*) > 0 from
  (select pg_tablespace_databases(oid) as pts from pg_tablespace
   where spcname = 'pg_default') pts
  join pg_database db on pts.pts = db.oid;

--
-- Test replication slot directory functions
--
CREATE ROLE regress_slot_dir_funcs;
-- Not available by default.
SELECT has_function_privilege('regress_slot_dir_funcs',
  'pg_ls_logicalsnapdir()', 'EXECUTE');
SELECT has_function_privilege('regress_slot_dir_funcs',
  'pg_ls_logicalmapdir()', 'EXECUTE');
SELECT has_function_privilege('regress_slot_dir_funcs',
  'pg_ls_replslotdir(text)', 'EXECUTE');
GRANT pg_monitor TO regress_slot_dir_funcs;
-- Role is now part of pg_monitor, so these are available.
SELECT has_function_privilege('regress_slot_dir_funcs',
  'pg_ls_logicalsnapdir()', 'EXECUTE');
SELECT has_function_privilege('regress_slot_dir_funcs',
  'pg_ls_logicalmapdir()', 'EXECUTE');
SELECT has_function_privilege('regress_slot_dir_funcs',
  'pg_ls_replslotdir(text)', 'EXECUTE');
DROP ROLE regress_slot_dir_funcs;

--
-- Test adding a support function to a subject function
--

CREATE FUNCTION my_int_eq(int, int) RETURNS bool
  LANGUAGE internal STRICT IMMUTABLE PARALLEL SAFE
  AS $$int4eq$$;

-- By default, planner does not think that's selective
EXPLAIN (COSTS OFF)
SELECT * FROM tenk1 a JOIN tenk1 b ON a.unique1 = b.unique1
WHERE my_int_eq(a.unique2, 42);

-- With support function that knows it's int4eq, we get a different plan
CREATE FUNCTION test_support_func(internal)
    RETURNS internal
    AS :'regresslib', 'test_support_func'
    LANGUAGE C STRICT;

ALTER FUNCTION my_int_eq(int, int) SUPPORT test_support_func;

EXPLAIN (COSTS OFF)
SELECT * FROM tenk1 a JOIN tenk1 b ON a.unique1 = b.unique1
WHERE my_int_eq(a.unique2, 42);

-- Also test non-default rowcount estimate
CREATE FUNCTION my_gen_series(int, int) RETURNS SETOF integer
  LANGUAGE internal STRICT IMMUTABLE PARALLEL SAFE
  AS $$generate_series_int4$$
  SUPPORT test_support_func;

EXPLAIN (COSTS OFF)
SELECT * FROM tenk1 a JOIN my_gen_series(1,1000) g ON a.unique1 = g;

EXPLAIN (COSTS OFF)
SELECT * FROM tenk1 a JOIN my_gen_series(1,10) g ON a.unique1 = g;

--
-- Test the SupportRequestRows support function for generate_series_timestamp()
--

-- Ensure the row estimate matches the actual rows
SELECT explain_mask_costs($$
SELECT * FROM generate_series(TIMESTAMPTZ '2024-02-01', TIMESTAMPTZ '2024-03-01', INTERVAL '1 day') g(s);$$,
true, true, false, true);

-- As above but with generate_series_timestamp
SELECT explain_mask_costs($$
SELECT * FROM generate_series(TIMESTAMP '2024-02-01', TIMESTAMP '2024-03-01', INTERVAL '1 day') g(s);$$,
true, true, false, true);

-- As above but with generate_series_timestamptz_at_zone()
SELECT explain_mask_costs($$
SELECT * FROM generate_series(TIMESTAMPTZ '2024-02-01', TIMESTAMPTZ '2024-03-01', INTERVAL '1 day', 'UTC') g(s);$$,
true, true, false, true);

-- Ensure the estimated and actual row counts match when the range isn't
-- evenly divisible by the step
SELECT explain_mask_costs($$
SELECT * FROM generate_series(TIMESTAMPTZ '2024-02-01', TIMESTAMPTZ '2024-03-01', INTERVAL '7 day') g(s);$$,
true, true, false, true);

-- Ensure the estimates match when step is decreasing
SELECT explain_mask_costs($$
SELECT * FROM generate_series(TIMESTAMPTZ '2024-03-01', TIMESTAMPTZ '2024-02-01', INTERVAL '-1 day') g(s);$$,
true, true, false, true);

-- Ensure an empty range estimates 1 row
SELECT explain_mask_costs($$
SELECT * FROM generate_series(TIMESTAMPTZ '2024-03-01', TIMESTAMPTZ '2024-02-01', INTERVAL '1 day') g(s);$$,
true, true, false, true);

-- Ensure we get the default row estimate for infinity values
SELECT explain_mask_costs($$
SELECT * FROM generate_series(TIMESTAMPTZ '-infinity', TIMESTAMPTZ 'infinity', INTERVAL '1 day') g(s);$$,
false, true, false, true);

-- Ensure the row estimate behaves correctly when step size is zero.
-- We expect generate_series_timestamp() to throw the error rather than in
-- the support function.
SELECT * FROM generate_series(TIMESTAMPTZ '2024-02-01', TIMESTAMPTZ '2024-03-01', INTERVAL '0 day') g(s);

--
-- Test the SupportRequestRows support function for generate_series_numeric()
--

-- Ensure the row estimate matches the actual rows
SELECT explain_mask_costs($$
SELECT * FROM generate_series(1.0, 25.0) g(s);$$,
true, true, false, true);

-- As above but with non-default step
SELECT explain_mask_costs($$
SELECT * FROM generate_series(1.0, 25.0, 2.0) g(s);$$,
true, true, false, true);

-- Ensure the estimates match when step is decreasing
SELECT explain_mask_costs($$
SELECT * FROM generate_series(25.0, 1.0, -1.0) g(s);$$,
true, true, false, true);

-- Ensure an empty range estimates 1 row
SELECT explain_mask_costs($$
SELECT * FROM generate_series(25.0, 1.0, 1.0) g(s);$$,
true, true, false, true);

-- Ensure we get the default row estimate for error cases (infinity/NaN values
-- and zero step size)
SELECT explain_mask_costs($$
SELECT * FROM generate_series('-infinity'::NUMERIC, 'infinity'::NUMERIC, 1.0) g(s);$$,
false, true, false, true);

SELECT explain_mask_costs($$
SELECT * FROM generate_series(1.0, 25.0, 'NaN'::NUMERIC) g(s);$$,
false, true, false, true);

SELECT explain_mask_costs($$
SELECT * FROM generate_series(25.0, 2.0, 0.0) g(s);$$,
false, true, false, true);

-- Test functions for control data
SELECT count(*) > 0 AS ok FROM pg_control_checkpoint();
SELECT count(*) > 0 AS ok FROM pg_control_init();
SELECT count(*) > 0 AS ok FROM pg_control_recovery();
SELECT count(*) > 0 AS ok FROM pg_control_system();

-- pg_split_walfile_name, pg_walfile_name & pg_walfile_name_offset
SELECT * FROM pg_split_walfile_name(NULL);
SELECT * FROM pg_split_walfile_name('invalid');
SELECT segment_number > 0 AS ok_segment_number, timeline_id
  FROM pg_split_walfile_name('000000010000000100000000');
SELECT segment_number > 0 AS ok_segment_number, timeline_id
  FROM pg_split_walfile_name('ffffffFF00000001000000af');
SELECT setting::int8 AS segment_size
FROM pg_settings
WHERE name = 'wal_segment_size'
\gset
SELECT segment_number, file_offset
FROM pg_walfile_name_offset('0/0'::pg_lsn + :segment_size),
     pg_split_walfile_name(file_name);
SELECT segment_number, file_offset
FROM pg_walfile_name_offset('0/0'::pg_lsn + :segment_size + 1),
     pg_split_walfile_name(file_name);
SELECT segment_number, file_offset = :segment_size - 1
FROM pg_walfile_name_offset('0/0'::pg_lsn + :segment_size - 1),
     pg_split_walfile_name(file_name);

-- pg_current_logfile
CREATE ROLE regress_current_logfile;
-- not available by default
SELECT has_function_privilege('regress_current_logfile',
  'pg_current_logfile()', 'EXECUTE');
GRANT pg_monitor TO regress_current_logfile;
-- role has privileges of pg_monitor and can execute the function
SELECT has_function_privilege('regress_current_logfile',
  'pg_current_logfile()', 'EXECUTE');
DROP ROLE regress_current_logfile;

-- pg_column_toast_chunk_id
CREATE TABLE test_chunk_id (a TEXT, b TEXT STORAGE EXTERNAL);
INSERT INTO test_chunk_id VALUES ('x', repeat('x', 8192));
SELECT t.relname AS toastrel FROM pg_class c
  LEFT JOIN pg_class t ON c.reltoastrelid = t.oid
  WHERE c.relname = 'test_chunk_id'
\gset
SELECT pg_column_toast_chunk_id(a) IS NULL,
  pg_column_toast_chunk_id(b) IN (SELECT chunk_id FROM pg_toast.:toastrel)
  FROM test_chunk_id;
DROP TABLE test_chunk_id;
DROP FUNCTION explain_mask_costs(text, bool, bool, bool, bool);

-- test stratnum support functions
SELECT gist_stratnum_common(7);
SELECT gist_stratnum_common(3);


-- relpath tests
CREATE FUNCTION test_relpath()
    RETURNS void
    AS :'regresslib'
    LANGUAGE C;
SELECT test_relpath();
