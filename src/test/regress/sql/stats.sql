--
-- Test cumulative stats system
--
-- Must be run after tenk2 has been created (by create_table),
-- populated (by create_misc) and indexed (by create_index).
--

-- conditio sine qua non
SHOW track_counts;  -- must be on

-- List of backend types, contexts and objects tracked in pg_stat_io.
\a
SELECT backend_type, object, context FROM pg_stat_io
  ORDER BY backend_type COLLATE "C", object COLLATE "C", context COLLATE "C";
\a

-- ensure that both seqscan and indexscan plans are allowed
SET enable_seqscan TO on;
SET enable_indexscan TO on;
-- for the moment, we don't want index-only scans here
SET enable_indexonlyscan TO off;
-- not enabled by default, but we want to test it...
SET track_functions TO 'all';

-- record dboid for later use
SELECT oid AS dboid from pg_database where datname = current_database() \gset

-- save counters
BEGIN;
SET LOCAL stats_fetch_consistency = snapshot;
CREATE TABLE prevstats AS
SELECT t.seq_scan, t.seq_tup_read, t.idx_scan, t.idx_tup_fetch,
       (b.heap_blks_read + b.heap_blks_hit) AS heap_blks,
       (b.idx_blks_read + b.idx_blks_hit) AS idx_blks,
       pg_stat_get_snapshot_timestamp() as snap_ts
  FROM pg_catalog.pg_stat_user_tables AS t,
       pg_catalog.pg_statio_user_tables AS b
 WHERE t.relname='tenk2' AND b.relname='tenk2';
COMMIT;

-- test effects of TRUNCATE on n_live_tup/n_dead_tup counters
CREATE TABLE trunc_stats_test(id serial);
CREATE TABLE trunc_stats_test1(id serial, stuff text);
CREATE TABLE trunc_stats_test2(id serial);
CREATE TABLE trunc_stats_test3(id serial, stuff text);
CREATE TABLE trunc_stats_test4(id serial);

-- check that n_live_tup is reset to 0 after truncate
INSERT INTO trunc_stats_test DEFAULT VALUES;
INSERT INTO trunc_stats_test DEFAULT VALUES;
INSERT INTO trunc_stats_test DEFAULT VALUES;
TRUNCATE trunc_stats_test;

-- test involving a truncate in a transaction; 4 ins but only 1 live
INSERT INTO trunc_stats_test1 DEFAULT VALUES;
INSERT INTO trunc_stats_test1 DEFAULT VALUES;
INSERT INTO trunc_stats_test1 DEFAULT VALUES;
UPDATE trunc_stats_test1 SET id = id + 10 WHERE id IN (1, 2);
DELETE FROM trunc_stats_test1 WHERE id = 3;

BEGIN;
UPDATE trunc_stats_test1 SET id = id + 100;
TRUNCATE trunc_stats_test1;
INSERT INTO trunc_stats_test1 DEFAULT VALUES;
COMMIT;

-- use a savepoint: 1 insert, 1 live
BEGIN;
INSERT INTO trunc_stats_test2 DEFAULT VALUES;
INSERT INTO trunc_stats_test2 DEFAULT VALUES;
SAVEPOINT p1;
INSERT INTO trunc_stats_test2 DEFAULT VALUES;
TRUNCATE trunc_stats_test2;
INSERT INTO trunc_stats_test2 DEFAULT VALUES;
RELEASE SAVEPOINT p1;
COMMIT;

-- rollback a savepoint: this should count 4 inserts and have 2
-- live tuples after commit (and 2 dead ones due to aborted subxact)
BEGIN;
INSERT INTO trunc_stats_test3 DEFAULT VALUES;
INSERT INTO trunc_stats_test3 DEFAULT VALUES;
SAVEPOINT p1;
INSERT INTO trunc_stats_test3 DEFAULT VALUES;
INSERT INTO trunc_stats_test3 DEFAULT VALUES;
TRUNCATE trunc_stats_test3;
INSERT INTO trunc_stats_test3 DEFAULT VALUES;
ROLLBACK TO SAVEPOINT p1;
COMMIT;

-- rollback a truncate: this should count 2 inserts and produce 2 dead tuples
BEGIN;
INSERT INTO trunc_stats_test4 DEFAULT VALUES;
INSERT INTO trunc_stats_test4 DEFAULT VALUES;
TRUNCATE trunc_stats_test4;
INSERT INTO trunc_stats_test4 DEFAULT VALUES;
ROLLBACK;

-- do a seqscan
SELECT count(*) FROM tenk2;
-- do an indexscan
-- make sure it is not a bitmap scan, which might skip fetching heap tuples
SET enable_bitmapscan TO off;
SELECT count(*) FROM tenk2 WHERE unique1 = 1;
RESET enable_bitmapscan;

-- ensure pending stats are flushed
SELECT pg_stat_force_next_flush();

-- check effects
BEGIN;
SET LOCAL stats_fetch_consistency = snapshot;

SELECT relname, n_tup_ins, n_tup_upd, n_tup_del, n_live_tup, n_dead_tup
  FROM pg_stat_user_tables
 WHERE relname like 'trunc_stats_test%' order by relname;

SELECT st.seq_scan >= pr.seq_scan + 1,
       st.seq_tup_read >= pr.seq_tup_read + cl.reltuples,
       st.idx_scan >= pr.idx_scan + 1,
       st.idx_tup_fetch >= pr.idx_tup_fetch + 1
  FROM pg_stat_user_tables AS st, pg_class AS cl, prevstats AS pr
 WHERE st.relname='tenk2' AND cl.relname='tenk2';

SELECT st.heap_blks_read + st.heap_blks_hit >= pr.heap_blks + cl.relpages,
       st.idx_blks_read + st.idx_blks_hit >= pr.idx_blks + 1
  FROM pg_statio_user_tables AS st, pg_class AS cl, prevstats AS pr
 WHERE st.relname='tenk2' AND cl.relname='tenk2';

SELECT pr.snap_ts < pg_stat_get_snapshot_timestamp() as snapshot_newer
FROM prevstats AS pr;

COMMIT;

----
-- Basic tests for track_functions
---
CREATE FUNCTION stats_test_func1() RETURNS VOID LANGUAGE plpgsql AS $$BEGIN END;$$;
SELECT 'stats_test_func1()'::regprocedure::oid AS stats_test_func1_oid \gset
CREATE FUNCTION stats_test_func2() RETURNS VOID LANGUAGE plpgsql AS $$BEGIN END;$$;
SELECT 'stats_test_func2()'::regprocedure::oid AS stats_test_func2_oid \gset

-- test that stats are accumulated
BEGIN;
SET LOCAL stats_fetch_consistency = none;
SELECT pg_stat_get_function_calls(:stats_test_func1_oid);
SELECT pg_stat_get_xact_function_calls(:stats_test_func1_oid);
SELECT stats_test_func1();
SELECT pg_stat_get_xact_function_calls(:stats_test_func1_oid);
SELECT stats_test_func1();
SELECT pg_stat_get_xact_function_calls(:stats_test_func1_oid);
SELECT pg_stat_get_function_calls(:stats_test_func1_oid);
COMMIT;

-- Verify that function stats are not transactional

-- rolled back savepoint in committing transaction
BEGIN;
SELECT stats_test_func2();
SAVEPOINT foo;
SELECT stats_test_func2();
ROLLBACK TO SAVEPOINT foo;
SELECT pg_stat_get_xact_function_calls(:stats_test_func2_oid);
SELECT stats_test_func2();
COMMIT;

-- rolled back transaction
BEGIN;
SELECT stats_test_func2();
ROLLBACK;

SELECT pg_stat_force_next_flush();

-- check collected stats
SELECT funcname, calls FROM pg_stat_user_functions WHERE funcid = :stats_test_func1_oid;
SELECT funcname, calls FROM pg_stat_user_functions WHERE funcid = :stats_test_func2_oid;


-- check that a rolled back drop function stats leaves stats alive
BEGIN;
SELECT funcname, calls FROM pg_stat_user_functions WHERE funcid = :stats_test_func1_oid;
DROP FUNCTION stats_test_func1();
-- shouldn't be visible via view
SELECT funcname, calls FROM pg_stat_user_functions WHERE funcid = :stats_test_func1_oid;
-- but still via oid access
SELECT pg_stat_get_function_calls(:stats_test_func1_oid);
ROLLBACK;
SELECT funcname, calls FROM pg_stat_user_functions WHERE funcid = :stats_test_func1_oid;
SELECT pg_stat_get_function_calls(:stats_test_func1_oid);


-- check that function dropped in main transaction leaves no stats behind
BEGIN;
DROP FUNCTION stats_test_func1();
COMMIT;
SELECT funcname, calls FROM pg_stat_user_functions WHERE funcid = :stats_test_func1_oid;
SELECT pg_stat_get_function_calls(:stats_test_func1_oid);

-- check that function dropped in a subtransaction leaves no stats behind
BEGIN;
SELECT stats_test_func2();
SAVEPOINT a;
SELECT stats_test_func2();
SAVEPOINT b;
DROP FUNCTION stats_test_func2();
COMMIT;
SELECT funcname, calls FROM pg_stat_user_functions WHERE funcid = :stats_test_func2_oid;
SELECT pg_stat_get_function_calls(:stats_test_func2_oid);


-- Check that stats for relations are dropped. For that we need to access stats
-- by oid after the DROP TABLE. Save oids.
CREATE TABLE drop_stats_test();
INSERT INTO drop_stats_test DEFAULT VALUES;
SELECT 'drop_stats_test'::regclass::oid AS drop_stats_test_oid \gset

CREATE TABLE drop_stats_test_xact();
INSERT INTO drop_stats_test_xact DEFAULT VALUES;
SELECT 'drop_stats_test_xact'::regclass::oid AS drop_stats_test_xact_oid \gset

CREATE TABLE drop_stats_test_subxact();
INSERT INTO drop_stats_test_subxact DEFAULT VALUES;
SELECT 'drop_stats_test_subxact'::regclass::oid AS drop_stats_test_subxact_oid \gset

SELECT pg_stat_force_next_flush();

SELECT pg_stat_get_live_tuples(:drop_stats_test_oid);
DROP TABLE drop_stats_test;
SELECT pg_stat_get_live_tuples(:drop_stats_test_oid);
SELECT pg_stat_get_xact_tuples_inserted(:drop_stats_test_oid);

-- check that rollback protects against having stats dropped and that local
-- modifications don't pose a problem
SELECT pg_stat_get_live_tuples(:drop_stats_test_xact_oid);
SELECT pg_stat_get_tuples_inserted(:drop_stats_test_xact_oid);
SELECT pg_stat_get_xact_tuples_inserted(:drop_stats_test_xact_oid);
BEGIN;
INSERT INTO drop_stats_test_xact DEFAULT VALUES;
SELECT pg_stat_get_xact_tuples_inserted(:drop_stats_test_xact_oid);
DROP TABLE drop_stats_test_xact;
SELECT pg_stat_get_xact_tuples_inserted(:drop_stats_test_xact_oid);
ROLLBACK;
SELECT pg_stat_force_next_flush();
SELECT pg_stat_get_live_tuples(:drop_stats_test_xact_oid);
SELECT pg_stat_get_tuples_inserted(:drop_stats_test_xact_oid);

-- transactional drop
SELECT pg_stat_get_live_tuples(:drop_stats_test_xact_oid);
SELECT pg_stat_get_tuples_inserted(:drop_stats_test_xact_oid);
BEGIN;
INSERT INTO drop_stats_test_xact DEFAULT VALUES;
SELECT pg_stat_get_xact_tuples_inserted(:drop_stats_test_xact_oid);
DROP TABLE drop_stats_test_xact;
SELECT pg_stat_get_xact_tuples_inserted(:drop_stats_test_xact_oid);
COMMIT;
SELECT pg_stat_force_next_flush();
SELECT pg_stat_get_live_tuples(:drop_stats_test_xact_oid);
SELECT pg_stat_get_tuples_inserted(:drop_stats_test_xact_oid);

-- savepoint rollback (2 levels)
SELECT pg_stat_get_live_tuples(:drop_stats_test_subxact_oid);
BEGIN;
INSERT INTO drop_stats_test_subxact DEFAULT VALUES;
SAVEPOINT sp1;
INSERT INTO drop_stats_test_subxact DEFAULT VALUES;
SELECT pg_stat_get_xact_tuples_inserted(:drop_stats_test_subxact_oid);
SAVEPOINT sp2;
DROP TABLE drop_stats_test_subxact;
ROLLBACK TO SAVEPOINT sp2;
SELECT pg_stat_get_xact_tuples_inserted(:drop_stats_test_subxact_oid);
COMMIT;
SELECT pg_stat_force_next_flush();
SELECT pg_stat_get_live_tuples(:drop_stats_test_subxact_oid);

-- savepoint rolback (1 level)
SELECT pg_stat_get_live_tuples(:drop_stats_test_subxact_oid);
BEGIN;
SAVEPOINT sp1;
DROP TABLE drop_stats_test_subxact;
SAVEPOINT sp2;
ROLLBACK TO SAVEPOINT sp1;
COMMIT;
SELECT pg_stat_get_live_tuples(:drop_stats_test_subxact_oid);

-- and now actually drop
SELECT pg_stat_get_live_tuples(:drop_stats_test_subxact_oid);
BEGIN;
SAVEPOINT sp1;
DROP TABLE drop_stats_test_subxact;
SAVEPOINT sp2;
RELEASE SAVEPOINT sp1;
COMMIT;
SELECT pg_stat_get_live_tuples(:drop_stats_test_subxact_oid);

DROP TABLE trunc_stats_test, trunc_stats_test1, trunc_stats_test2, trunc_stats_test3, trunc_stats_test4;
DROP TABLE prevstats;


-----
-- Test that last_seq_scan, last_idx_scan are correctly maintained
--
-- Perform test using a temporary table. That way autovacuum etc won't
-- interfere. To be able to check that timestamps increase, we sleep for 100ms
-- between tests, assuming that there aren't systems with a coarser timestamp
-- granularity.
-----

BEGIN;
CREATE TEMPORARY TABLE test_last_scan(idx_col int primary key, noidx_col int);
INSERT INTO test_last_scan(idx_col, noidx_col) VALUES(1, 1);
SELECT pg_stat_force_next_flush();
SELECT last_seq_scan, last_idx_scan FROM pg_stat_all_tables WHERE relid = 'test_last_scan'::regclass;
COMMIT;

SELECT pg_stat_reset_single_table_counters('test_last_scan'::regclass);
SELECT seq_scan, idx_scan FROM pg_stat_all_tables WHERE relid = 'test_last_scan'::regclass;

-- ensure we start out with exactly one index and sequential scan
BEGIN;
SET LOCAL enable_seqscan TO on;
SET LOCAL enable_indexscan TO on;
SET LOCAL enable_bitmapscan TO off;
EXPLAIN (COSTS off) SELECT count(*) FROM test_last_scan WHERE noidx_col = 1;
SELECT count(*) FROM test_last_scan WHERE noidx_col = 1;
SET LOCAL enable_seqscan TO off;
EXPLAIN (COSTS off) SELECT count(*) FROM test_last_scan WHERE idx_col = 1;
SELECT count(*) FROM test_last_scan WHERE idx_col = 1;
SELECT pg_stat_force_next_flush();
COMMIT;

-- fetch timestamps from before the next test
SELECT last_seq_scan AS test_last_seq, last_idx_scan AS test_last_idx
FROM pg_stat_all_tables WHERE relid = 'test_last_scan'::regclass \gset
SELECT pg_sleep(0.1); -- assume a minimum timestamp granularity of 100ms

-- cause one sequential scan
BEGIN;
SET LOCAL enable_seqscan TO on;
SET LOCAL enable_indexscan TO off;
SET LOCAL enable_bitmapscan TO off;
EXPLAIN (COSTS off) SELECT count(*) FROM test_last_scan WHERE noidx_col = 1;
SELECT count(*) FROM test_last_scan WHERE noidx_col = 1;
SELECT pg_stat_force_next_flush();
COMMIT;
-- check that just sequential scan stats were incremented
SELECT seq_scan, :'test_last_seq' < last_seq_scan AS seq_ok, idx_scan, :'test_last_idx' = last_idx_scan AS idx_ok
FROM pg_stat_all_tables WHERE relid = 'test_last_scan'::regclass;

-- fetch timestamps from before the next test
SELECT last_seq_scan AS test_last_seq, last_idx_scan AS test_last_idx
FROM pg_stat_all_tables WHERE relid = 'test_last_scan'::regclass \gset
SELECT pg_sleep(0.1);

-- cause one index scan
BEGIN;
SET LOCAL enable_seqscan TO off;
SET LOCAL enable_indexscan TO on;
SET LOCAL enable_bitmapscan TO off;
EXPLAIN (COSTS off) SELECT count(*) FROM test_last_scan WHERE idx_col = 1;
SELECT count(*) FROM test_last_scan WHERE idx_col = 1;
SELECT pg_stat_force_next_flush();
COMMIT;
-- check that just index scan stats were incremented
SELECT seq_scan, :'test_last_seq' = last_seq_scan AS seq_ok, idx_scan, :'test_last_idx' < last_idx_scan AS idx_ok
FROM pg_stat_all_tables WHERE relid = 'test_last_scan'::regclass;

-- fetch timestamps from before the next test
SELECT last_seq_scan AS test_last_seq, last_idx_scan AS test_last_idx
FROM pg_stat_all_tables WHERE relid = 'test_last_scan'::regclass \gset
SELECT pg_sleep(0.1);

-- cause one bitmap index scan
BEGIN;
SET LOCAL enable_seqscan TO off;
SET LOCAL enable_indexscan TO off;
SET LOCAL enable_bitmapscan TO on;
EXPLAIN (COSTS off) SELECT count(*) FROM test_last_scan WHERE idx_col = 1;
SELECT count(*) FROM test_last_scan WHERE idx_col = 1;
SELECT pg_stat_force_next_flush();
COMMIT;
-- check that just index scan stats were incremented
SELECT seq_scan, :'test_last_seq' = last_seq_scan AS seq_ok, idx_scan, :'test_last_idx' < last_idx_scan AS idx_ok
FROM pg_stat_all_tables WHERE relid = 'test_last_scan'::regclass;

-----
-- Test reset of some stats for shared table
-----

-- This updates the comment of the database currently in use in
-- pg_shdescription with a fake value, then sets it back to its
-- original value.
SELECT shobj_description(d.oid, 'pg_database') as description_before
  FROM pg_database d WHERE datname = current_database() \gset

-- force some stats in pg_shdescription.
BEGIN;
SELECT current_database() as datname \gset
COMMENT ON DATABASE :"datname" IS 'This is a test comment';
SELECT pg_stat_force_next_flush();
COMMIT;

-- check that the stats are reset.
SELECT (n_tup_ins + n_tup_upd) > 0 AS has_data FROM pg_stat_all_tables
  WHERE relid = 'pg_shdescription'::regclass;
SELECT pg_stat_reset_single_table_counters('pg_shdescription'::regclass);
SELECT (n_tup_ins + n_tup_upd) > 0 AS has_data FROM pg_stat_all_tables
  WHERE relid = 'pg_shdescription'::regclass;

-- set back comment
\if :{?description_before}
  COMMENT ON DATABASE :"datname" IS :'description_before';
\else
  COMMENT ON DATABASE :"datname" IS NULL;
\endif

-----
-- Test that various stats views are being properly populated
-----

-- Test that sessions is incremented when a new session is started in pg_stat_database
SELECT sessions AS db_stat_sessions FROM pg_stat_database WHERE datname = (SELECT current_database()) \gset
\c
SELECT pg_stat_force_next_flush();
SELECT sessions > :db_stat_sessions FROM pg_stat_database WHERE datname = (SELECT current_database());

-- Test pg_stat_checkpointer checkpointer-related stats, together with pg_stat_wal
SELECT num_requested AS rqst_ckpts_before FROM pg_stat_checkpointer \gset

-- Test pg_stat_wal
SELECT wal_bytes AS wal_bytes_before FROM pg_stat_wal \gset

-- Test pg_stat_get_backend_wal()
SELECT wal_bytes AS backend_wal_bytes_before from pg_stat_get_backend_wal(pg_backend_pid()) \gset

-- Make a temp table so our temp schema exists
CREATE TEMP TABLE test_stats_temp AS SELECT 17;
DROP TABLE test_stats_temp;

-- Checkpoint twice: The checkpointer reports stats after reporting completion
-- of the checkpoint. But after a second checkpoint we'll see at least the
-- results of the first.
CHECKPOINT;
CHECKPOINT;

SELECT num_requested > :rqst_ckpts_before FROM pg_stat_checkpointer;
SELECT wal_bytes > :wal_bytes_before FROM pg_stat_wal;

SELECT pg_stat_force_next_flush();
SELECT wal_bytes > :backend_wal_bytes_before FROM pg_stat_get_backend_wal(pg_backend_pid());

-- Test pg_stat_get_backend_idset() and some allied functions.
-- In particular, verify that their notion of backend ID matches
-- our temp schema index.
SELECT (current_schemas(true))[1] = ('pg_temp_' || beid::text) AS match
FROM pg_stat_get_backend_idset() beid
WHERE pg_stat_get_backend_pid(beid) = pg_backend_pid();

-----
-- Test that resetting stats works for reset timestamp
-----

-- Test that reset_slru with a specified SLRU works.
SELECT stats_reset AS slru_commit_ts_reset_ts FROM pg_stat_slru WHERE name = 'commit_timestamp' \gset
SELECT stats_reset AS slru_notify_reset_ts FROM pg_stat_slru WHERE name = 'notify' \gset
SELECT pg_stat_reset_slru('commit_timestamp');
SELECT stats_reset > :'slru_commit_ts_reset_ts'::timestamptz FROM pg_stat_slru WHERE name = 'commit_timestamp';
SELECT stats_reset AS slru_commit_ts_reset_ts FROM pg_stat_slru WHERE name = 'commit_timestamp' \gset

-- Test that multiple SLRUs are reset when no specific SLRU provided to reset function
SELECT pg_stat_reset_slru();
SELECT stats_reset > :'slru_commit_ts_reset_ts'::timestamptz FROM pg_stat_slru WHERE name = 'commit_timestamp';
SELECT stats_reset > :'slru_notify_reset_ts'::timestamptz FROM pg_stat_slru WHERE name = 'notify';

-- Test that reset_shared with archiver specified as the stats type works
SELECT stats_reset AS archiver_reset_ts FROM pg_stat_archiver \gset
SELECT pg_stat_reset_shared('archiver');
SELECT stats_reset > :'archiver_reset_ts'::timestamptz FROM pg_stat_archiver;

-- Test that reset_shared with bgwriter specified as the stats type works
SELECT stats_reset AS bgwriter_reset_ts FROM pg_stat_bgwriter \gset
SELECT pg_stat_reset_shared('bgwriter');
SELECT stats_reset > :'bgwriter_reset_ts'::timestamptz FROM pg_stat_bgwriter;

-- Test that reset_shared with checkpointer specified as the stats type works
SELECT stats_reset AS checkpointer_reset_ts FROM pg_stat_checkpointer \gset
SELECT pg_stat_reset_shared('checkpointer');
SELECT stats_reset > :'checkpointer_reset_ts'::timestamptz FROM pg_stat_checkpointer;

-- Test that reset_shared with recovery_prefetch specified as the stats type works
SELECT stats_reset AS recovery_prefetch_reset_ts FROM pg_stat_recovery_prefetch \gset
SELECT pg_stat_reset_shared('recovery_prefetch');
SELECT stats_reset > :'recovery_prefetch_reset_ts'::timestamptz FROM pg_stat_recovery_prefetch;

-- Test that reset_shared with slru specified as the stats type works
SELECT max(stats_reset) AS slru_reset_ts FROM pg_stat_slru \gset
SELECT pg_stat_reset_shared('slru');
SELECT max(stats_reset) > :'slru_reset_ts'::timestamptz FROM pg_stat_slru;

-- Test that reset_shared with wal specified as the stats type works
SELECT stats_reset AS wal_reset_ts FROM pg_stat_wal \gset
SELECT pg_stat_reset_shared('wal');
SELECT stats_reset > :'wal_reset_ts'::timestamptz FROM pg_stat_wal;

-- Test error case for reset_shared with unknown stats type
SELECT pg_stat_reset_shared('unknown');

-- Test that reset works for pg_stat_database

-- Since pg_stat_database stats_reset starts out as NULL, reset it once first so we have something to compare it to
SELECT pg_stat_reset();
SELECT stats_reset AS db_reset_ts FROM pg_stat_database WHERE datname = (SELECT current_database()) \gset
SELECT pg_stat_reset();
SELECT stats_reset > :'db_reset_ts'::timestamptz FROM pg_stat_database WHERE datname = (SELECT current_database());


----
-- pg_stat_get_snapshot_timestamp behavior
----
BEGIN;
SET LOCAL stats_fetch_consistency = snapshot;
-- no snapshot yet, return NULL
SELECT pg_stat_get_snapshot_timestamp();
-- any attempt at accessing stats will build snapshot
SELECT pg_stat_get_function_calls(0);
SELECT pg_stat_get_snapshot_timestamp() >= NOW();
-- shows NULL again after clearing
SELECT pg_stat_clear_snapshot();
SELECT pg_stat_get_snapshot_timestamp();
COMMIT;

----
-- Changing stats_fetch_consistency in a transaction.
----
BEGIN;
-- Stats filled under the cache mode
SET LOCAL stats_fetch_consistency = cache;
SELECT pg_stat_get_function_calls(0);
SELECT pg_stat_get_snapshot_timestamp() IS NOT NULL AS snapshot_ok;
-- Success in accessing pre-existing snapshot data.
SET LOCAL stats_fetch_consistency = snapshot;
SELECT pg_stat_get_snapshot_timestamp() IS NOT NULL AS snapshot_ok;
SELECT pg_stat_get_function_calls(0);
SELECT pg_stat_get_snapshot_timestamp() IS NOT NULL AS snapshot_ok;
-- Snapshot cleared.
SET LOCAL stats_fetch_consistency = none;
SELECT pg_stat_get_snapshot_timestamp() IS NOT NULL AS snapshot_ok;
SELECT pg_stat_get_function_calls(0);
SELECT pg_stat_get_snapshot_timestamp() IS NOT NULL AS snapshot_ok;
ROLLBACK;

----
-- pg_stat_have_stats behavior
----
-- fixed-numbered stats exist
SELECT pg_stat_have_stats('bgwriter', 0, 0);
-- unknown stats kinds error out
SELECT pg_stat_have_stats('zaphod', 0, 0);
-- db stats have objid 0
SELECT pg_stat_have_stats('database', :dboid, 1);
SELECT pg_stat_have_stats('database', :dboid, 0);

-- pg_stat_have_stats returns true for committed index creation
CREATE table stats_test_tab1 as select generate_series(1,10) a;
CREATE index stats_test_idx1 on stats_test_tab1(a);
SELECT 'stats_test_idx1'::regclass::oid AS stats_test_idx1_oid \gset
SET enable_seqscan TO off;
select a from stats_test_tab1 where a = 3;
SELECT pg_stat_have_stats('relation', :dboid, :stats_test_idx1_oid);

-- pg_stat_have_stats returns false for dropped index with stats
SELECT pg_stat_have_stats('relation', :dboid, :stats_test_idx1_oid);
DROP index stats_test_idx1;
SELECT pg_stat_have_stats('relation', :dboid, :stats_test_idx1_oid);

-- pg_stat_have_stats returns false for rolled back index creation
BEGIN;
CREATE index stats_test_idx1 on stats_test_tab1(a);
SELECT 'stats_test_idx1'::regclass::oid AS stats_test_idx1_oid \gset
select a from stats_test_tab1 where a = 3;
SELECT pg_stat_have_stats('relation', :dboid, :stats_test_idx1_oid);
ROLLBACK;
SELECT pg_stat_have_stats('relation', :dboid, :stats_test_idx1_oid);

-- pg_stat_have_stats returns true for reindex CONCURRENTLY
CREATE index stats_test_idx1 on stats_test_tab1(a);
SELECT 'stats_test_idx1'::regclass::oid AS stats_test_idx1_oid \gset
select a from stats_test_tab1 where a = 3;
SELECT pg_stat_have_stats('relation', :dboid, :stats_test_idx1_oid);
REINDEX index CONCURRENTLY stats_test_idx1;
-- false for previous oid
SELECT pg_stat_have_stats('relation', :dboid, :stats_test_idx1_oid);
-- true for new oid
SELECT 'stats_test_idx1'::regclass::oid AS stats_test_idx1_oid \gset
SELECT pg_stat_have_stats('relation', :dboid, :stats_test_idx1_oid);

-- pg_stat_have_stats returns true for a rolled back drop index with stats
BEGIN;
SELECT pg_stat_have_stats('relation', :dboid, :stats_test_idx1_oid);
DROP index stats_test_idx1;
ROLLBACK;
SELECT pg_stat_have_stats('relation', :dboid, :stats_test_idx1_oid);

-- put enable_seqscan back to on
SET enable_seqscan TO on;

-- ensure that stats accessors handle NULL input correctly
SELECT pg_stat_get_replication_slot(NULL);
SELECT pg_stat_get_subscription_stats(NULL);


-- Test that the following operations are tracked in pg_stat_io and in
-- backend stats:
-- - reads of target blocks into shared buffers
-- - writes of shared buffers to permanent storage
-- - extends of relations using shared buffers
-- - fsyncs done to ensure the durability of data dirtying shared buffers
-- - shared buffer hits
-- - WAL writes and fsyncs in IOContext IOCONTEXT_NORMAL

-- There is no test for blocks evicted from shared buffers, because we cannot
-- be sure of the state of shared buffers at the point the test is run.

-- Create a regular table and insert some data to generate IOCONTEXT_NORMAL
-- extends.
SELECT pid AS checkpointer_pid FROM pg_stat_activity
  WHERE backend_type = 'checkpointer' \gset
SELECT sum(extends) AS io_sum_shared_before_extends
  FROM pg_stat_io WHERE context = 'normal' AND object = 'relation' \gset
SELECT sum(extends) AS my_io_sum_shared_before_extends
  FROM pg_stat_get_backend_io(pg_backend_pid())
  WHERE context = 'normal' AND object = 'relation' \gset
SELECT sum(writes) AS writes, sum(fsyncs) AS fsyncs
  FROM pg_stat_io
  WHERE object = 'relation' \gset io_sum_shared_before_
SELECT sum(writes) AS writes, sum(fsyncs) AS fsyncs
  FROM pg_stat_get_backend_io(pg_backend_pid())
  WHERE object = 'relation' \gset my_io_sum_shared_before_
SELECT sum(writes) AS writes, sum(fsyncs) AS fsyncs
  FROM pg_stat_io
  WHERE context = 'normal' AND object = 'wal' \gset io_sum_wal_normal_before_
CREATE TABLE test_io_shared(a int);
INSERT INTO test_io_shared SELECT i FROM generate_series(1,100)i;
SELECT pg_stat_force_next_flush();
SELECT sum(extends) AS io_sum_shared_after_extends
  FROM pg_stat_io WHERE context = 'normal' AND object = 'relation' \gset
SELECT :io_sum_shared_after_extends > :io_sum_shared_before_extends;
SELECT sum(extends) AS my_io_sum_shared_after_extends
  FROM pg_stat_get_backend_io(pg_backend_pid())
  WHERE context = 'normal' AND object = 'relation' \gset
SELECT :my_io_sum_shared_after_extends > :my_io_sum_shared_before_extends;

-- After a checkpoint, there should be some additional IOCONTEXT_NORMAL writes
-- and fsyncs in the global stats (usually not for the backend).
-- See comment above for rationale for two explicit CHECKPOINTs.
CHECKPOINT;
CHECKPOINT;
SELECT sum(writes) AS writes, sum(fsyncs) AS fsyncs
  FROM pg_stat_io
  WHERE object = 'relation' \gset io_sum_shared_after_
SELECT :io_sum_shared_after_writes > :io_sum_shared_before_writes;
SELECT current_setting('fsync') = 'off'
  OR :io_sum_shared_after_fsyncs > :io_sum_shared_before_fsyncs;
SELECT sum(writes) AS writes, sum(fsyncs) AS fsyncs
  FROM pg_stat_get_backend_io(pg_backend_pid())
  WHERE object = 'relation' \gset my_io_sum_shared_after_
SELECT :my_io_sum_shared_after_writes >= :my_io_sum_shared_before_writes;
SELECT current_setting('fsync') = 'off'
  OR :my_io_sum_shared_after_fsyncs >= :my_io_sum_shared_before_fsyncs;
SELECT sum(writes) AS writes, sum(fsyncs) AS fsyncs
  FROM pg_stat_io
  WHERE context = 'normal' AND object = 'wal' \gset io_sum_wal_normal_after_
SELECT current_setting('synchronous_commit') = 'on';
SELECT :io_sum_wal_normal_after_writes > :io_sum_wal_normal_before_writes;
SELECT current_setting('fsync') = 'off'
  OR current_setting('wal_sync_method') IN ('open_sync', 'open_datasync')
  OR :io_sum_wal_normal_after_fsyncs > :io_sum_wal_normal_before_fsyncs;

-- Change the tablespace so that the table is rewritten directly, then SELECT
-- from it to cause it to be read back into shared buffers.
SELECT sum(reads) AS io_sum_shared_before_reads
  FROM pg_stat_io WHERE context = 'normal' AND object = 'relation' \gset
-- Do this in a transaction to prevent spurious failures due to concurrent accesses to our newly
-- rewritten table, e.g. by autovacuum.
BEGIN;
ALTER TABLE test_io_shared SET TABLESPACE regress_tblspace;
-- SELECT from the table so that the data is read into shared buffers and
-- context 'normal', object 'relation' reads are counted.
SELECT COUNT(*) FROM test_io_shared;
COMMIT;
SELECT pg_stat_force_next_flush();
SELECT sum(reads) AS io_sum_shared_after_reads
  FROM pg_stat_io WHERE context = 'normal' AND object = 'relation'  \gset
SELECT :io_sum_shared_after_reads > :io_sum_shared_before_reads;

SELECT sum(hits) AS io_sum_shared_before_hits
  FROM pg_stat_io WHERE context = 'normal' AND object = 'relation' \gset
-- Select from the table again to count hits.
-- Ensure we generate hits by forcing a nested loop self-join with no
-- materialize node. The outer side's buffer will stay pinned, preventing its
-- eviction, while we loop through the inner side and generate hits.
BEGIN;
SET LOCAL enable_nestloop TO on; SET LOCAL enable_mergejoin TO off;
SET LOCAL enable_hashjoin TO off; SET LOCAL enable_material TO off;
-- ensure plan stays as we expect it to
EXPLAIN (COSTS OFF) SELECT COUNT(*) FROM test_io_shared t1 INNER JOIN test_io_shared t2 USING (a);
SELECT COUNT(*) FROM test_io_shared t1 INNER JOIN test_io_shared t2 USING (a);
COMMIT;
SELECT pg_stat_force_next_flush();
SELECT sum(hits) AS io_sum_shared_after_hits
  FROM pg_stat_io WHERE context = 'normal' AND object = 'relation' \gset
SELECT :io_sum_shared_after_hits > :io_sum_shared_before_hits;

DROP TABLE test_io_shared;

-- Test that the follow IOCONTEXT_LOCAL IOOps are tracked in pg_stat_io:
-- - eviction of local buffers in order to reuse them
-- - reads of temporary table blocks into local buffers
-- - writes of local buffers to permanent storage
-- - extends of temporary tables

-- Set temp_buffers to its minimum so that we can trigger writes with fewer
-- inserted tuples. Do so in a new session in case temporary tables have been
-- accessed by previous tests in this session.
\c
SET temp_buffers TO 100;
CREATE TEMPORARY TABLE test_io_local(a int, b TEXT);
SELECT sum(extends) AS extends, sum(evictions) AS evictions, sum(writes) AS writes
  FROM pg_stat_io
  WHERE context = 'normal' AND object = 'temp relation' \gset io_sum_local_before_
-- Insert tuples into the temporary table, generating extends in the stats.
-- Insert enough values that we need to reuse and write out dirty local
-- buffers, generating evictions and writes.
INSERT INTO test_io_local SELECT generate_series(1, 5000) as id, repeat('a', 200);
-- Ensure the table is large enough to exceed our temp_buffers setting.
SELECT pg_relation_size('test_io_local') / current_setting('block_size')::int8 > 100;

SELECT sum(reads) AS io_sum_local_before_reads
  FROM pg_stat_io WHERE context = 'normal' AND object = 'temp relation' \gset
-- Read in evicted buffers, generating reads.
SELECT COUNT(*) FROM test_io_local;
SELECT pg_stat_force_next_flush();
SELECT sum(evictions) AS evictions,
       sum(reads) AS reads,
       sum(writes) AS writes,
       sum(extends) AS extends
  FROM pg_stat_io
  WHERE context = 'normal' AND object = 'temp relation'  \gset io_sum_local_after_
SELECT :io_sum_local_after_evictions > :io_sum_local_before_evictions,
       :io_sum_local_after_reads > :io_sum_local_before_reads,
       :io_sum_local_after_writes > :io_sum_local_before_writes,
       :io_sum_local_after_extends > :io_sum_local_before_extends;

-- Change the tablespaces so that the temporary table is rewritten to other
-- local buffers, exercising a different codepath than standard local buffer
-- writes.
ALTER TABLE test_io_local SET TABLESPACE regress_tblspace;
SELECT pg_stat_force_next_flush();
SELECT sum(writes) AS io_sum_local_new_tblspc_writes
  FROM pg_stat_io WHERE context = 'normal' AND object = 'temp relation'  \gset
SELECT :io_sum_local_new_tblspc_writes > :io_sum_local_after_writes;
RESET temp_buffers;

-- Test that reuse of strategy buffers and reads of blocks into these reused
-- buffers while VACUUMing are tracked in pg_stat_io. If there is sufficient
-- demand for shared buffers from concurrent queries, some buffers may be
-- pinned by other backends before they can be reused. In such cases, the
-- backend will evict a buffer from outside the ring and add it to the
-- ring. This is considered an eviction and not a reuse.

-- Set wal_skip_threshold smaller than the expected size of
-- test_io_vac_strategy so that, even if wal_level is minimal, VACUUM FULL will
-- fsync the newly rewritten test_io_vac_strategy instead of writing it to WAL.
-- Writing it to WAL will result in the newly written relation pages being in
-- shared buffers -- preventing us from testing BAS_VACUUM BufferAccessStrategy
-- reads.
SET wal_skip_threshold = '1 kB';
SELECT sum(reuses) AS reuses, sum(reads) AS reads, sum(evictions) AS evictions
  FROM pg_stat_io WHERE context = 'vacuum' \gset io_sum_vac_strategy_before_
CREATE TABLE test_io_vac_strategy(a int, b int) WITH (autovacuum_enabled = 'false');
INSERT INTO test_io_vac_strategy SELECT i, i from generate_series(1, 4500)i;
-- Ensure that the next VACUUM will need to perform IO by rewriting the table
-- first with VACUUM (FULL).
VACUUM (FULL) test_io_vac_strategy;
-- Use the minimum BUFFER_USAGE_LIMIT to cause reuses or evictions with the
-- smallest table possible.
VACUUM (PARALLEL 0, BUFFER_USAGE_LIMIT 128) test_io_vac_strategy;
SELECT pg_stat_force_next_flush();
SELECT sum(reuses) AS reuses, sum(reads) AS reads, sum(evictions) AS evictions
  FROM pg_stat_io WHERE context = 'vacuum' \gset io_sum_vac_strategy_after_
SELECT :io_sum_vac_strategy_after_reads > :io_sum_vac_strategy_before_reads;
SELECT (:io_sum_vac_strategy_after_reuses + :io_sum_vac_strategy_after_evictions) >
  (:io_sum_vac_strategy_before_reuses + :io_sum_vac_strategy_before_evictions);
RESET wal_skip_threshold;

-- Test that extends done by a CTAS, which uses a BAS_BULKWRITE
-- BufferAccessStrategy, are tracked in pg_stat_io.
SELECT sum(extends) AS io_sum_bulkwrite_strategy_extends_before
  FROM pg_stat_io WHERE context = 'bulkwrite' \gset
CREATE TABLE test_io_bulkwrite_strategy AS SELECT i FROM generate_series(1,100)i;
SELECT pg_stat_force_next_flush();
SELECT sum(extends) AS io_sum_bulkwrite_strategy_extends_after
  FROM pg_stat_io WHERE context = 'bulkwrite' \gset
SELECT :io_sum_bulkwrite_strategy_extends_after > :io_sum_bulkwrite_strategy_extends_before;

-- Test IO stats reset
SELECT pg_stat_have_stats('io', 0, 0);
SELECT sum(evictions) + sum(reuses) + sum(extends) + sum(fsyncs) + sum(reads) + sum(writes) + sum(writebacks) + sum(hits) AS io_stats_pre_reset
  FROM pg_stat_io \gset
SELECT sum(evictions) + sum(reuses) + sum(extends) + sum(fsyncs) + sum(reads) + sum(writes) + sum(writebacks) + sum(hits) AS my_io_stats_pre_reset
  FROM pg_stat_get_backend_io(pg_backend_pid()) \gset
SELECT pg_stat_reset_shared('io');
SELECT sum(evictions) + sum(reuses) + sum(extends) + sum(fsyncs) + sum(reads) + sum(writes) + sum(writebacks) + sum(hits) AS io_stats_post_reset
  FROM pg_stat_io \gset
SELECT :io_stats_post_reset < :io_stats_pre_reset;
SELECT sum(evictions) + sum(reuses) + sum(extends) + sum(fsyncs) + sum(reads) + sum(writes) + sum(writebacks) + sum(hits) AS my_io_stats_post_reset
  FROM pg_stat_get_backend_io(pg_backend_pid()) \gset
-- pg_stat_reset_shared() did not reset backend IO stats
SELECT :my_io_stats_pre_reset <= :my_io_stats_post_reset;
-- but pg_stat_reset_backend_stats() does
SELECT pg_stat_reset_backend_stats(pg_backend_pid());
SELECT sum(evictions) + sum(reuses) + sum(extends) + sum(fsyncs) + sum(reads) + sum(writes) + sum(writebacks) + sum(hits) AS my_io_stats_post_backend_reset
  FROM pg_stat_get_backend_io(pg_backend_pid()) \gset
SELECT :my_io_stats_pre_reset > :my_io_stats_post_backend_reset;

-- Check invalid input for pg_stat_get_backend_io()
SELECT pg_stat_get_backend_io(NULL);
SELECT pg_stat_get_backend_io(0);
-- Auxiliary processes return no data.
SELECT pg_stat_get_backend_io(:checkpointer_pid);

-- test BRIN index doesn't block HOT update
CREATE TABLE brin_hot (
  id  integer PRIMARY KEY,
  val integer NOT NULL
) WITH (autovacuum_enabled = off, fillfactor = 70);

INSERT INTO brin_hot SELECT *, 0 FROM generate_series(1, 235);
CREATE INDEX val_brin ON brin_hot using brin(val);

CREATE FUNCTION wait_for_hot_stats() RETURNS void AS $$
DECLARE
  start_time timestamptz := clock_timestamp();
  updated bool;
BEGIN
  -- we don't want to wait forever; loop will exit after 30 seconds
  FOR i IN 1 .. 300 LOOP
    SELECT (pg_stat_get_tuples_hot_updated('brin_hot'::regclass::oid) > 0) INTO updated;
    EXIT WHEN updated;

    -- wait a little
    PERFORM pg_sleep_for('100 milliseconds');
    -- reset stats snapshot so we can test again
    PERFORM pg_stat_clear_snapshot();
  END LOOP;
  -- report time waited in postmaster log (where it won't change test output)
  RAISE log 'wait_for_hot_stats delayed % seconds',
    EXTRACT(epoch FROM clock_timestamp() - start_time);
END
$$ LANGUAGE plpgsql;

UPDATE brin_hot SET val = -3 WHERE id = 42;

-- We can't just call wait_for_hot_stats() at this point, because we only
-- transmit stats when the session goes idle, and we probably didn't
-- transmit the last couple of counts yet thanks to the rate-limiting logic
-- in pgstat_report_stat().  But instead of waiting for the rate limiter's
-- timeout to elapse, let's just start a new session.  The old one will
-- then send its stats before dying.
\c -

SELECT wait_for_hot_stats();
SELECT pg_stat_get_tuples_hot_updated('brin_hot'::regclass::oid);

DROP TABLE brin_hot;
DROP FUNCTION wait_for_hot_stats();

-- Test handling of index predicates - updating attributes in precicates
-- should not block HOT when summarizing indexes are involved. We update
-- a row that was not indexed due to the index predicate, and becomes
-- indexable - the HOT-updated tuple is forwarded to the BRIN index.
CREATE TABLE brin_hot_2 (a int, b int);
INSERT INTO brin_hot_2 VALUES (1, 100);
CREATE INDEX ON brin_hot_2 USING brin (b) WHERE a = 2;

UPDATE brin_hot_2 SET a = 2;

EXPLAIN (COSTS OFF) SELECT * FROM brin_hot_2 WHERE a = 2 AND b = 100;
SELECT COUNT(*) FROM brin_hot_2 WHERE a = 2 AND b = 100;

SET enable_seqscan = off;

EXPLAIN (COSTS OFF) SELECT * FROM brin_hot_2 WHERE a = 2 AND b = 100;
SELECT COUNT(*) FROM brin_hot_2 WHERE a = 2 AND b = 100;

DROP TABLE brin_hot_2;

-- Test that updates to indexed columns are still propagated to the
-- BRIN column.
-- https://postgr.es/m/05ebcb44-f383-86e3-4f31-0a97a55634cf@enterprisedb.com
CREATE TABLE brin_hot_3 (a int, filler text) WITH (fillfactor = 10);
INSERT INTO brin_hot_3 SELECT 1, repeat(' ', 500) FROM generate_series(1, 20);
CREATE INDEX ON brin_hot_3 USING brin (a) WITH (pages_per_range = 1);
UPDATE brin_hot_3 SET a = 2;

EXPLAIN (COSTS OFF) SELECT * FROM brin_hot_3 WHERE a = 2;
SELECT COUNT(*) FROM brin_hot_3 WHERE a = 2;

DROP TABLE brin_hot_3;

SET enable_seqscan = on;

-- Test that estimation of relation size works with tuples wider than the
-- relation fillfactor. We create a table with wide inline attributes and
-- low fillfactor, insert rows and then see how many rows EXPLAIN shows
-- before running analyze. We disable autovacuum so that it does not
-- interfere with the test.
CREATE TABLE table_fillfactor (
  n char(1000)
) with (fillfactor=10, autovacuum_enabled=off);

INSERT INTO table_fillfactor
SELECT 'x' FROM generate_series(1,1000);

SELECT * FROM check_estimated_rows('SELECT * FROM table_fillfactor');

DROP TABLE table_fillfactor;

-- End of Stats Test
