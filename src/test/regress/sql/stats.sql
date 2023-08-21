--
-- Test cumulative stats system
--
-- Must be run after tenk2 has been created (by create_table),
-- populated (by create_misc) and indexed (by create_index).
--

-- conditio sine qua non
SHOW track_counts;  -- must be on

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

-- Test pg_stat_bgwriter checkpointer-related stats, together with pg_stat_wal
SELECT checkpoints_req AS rqst_ckpts_before FROM pg_stat_bgwriter \gset

-- Test pg_stat_wal
SELECT wal_bytes AS wal_bytes_before FROM pg_stat_wal \gset

CREATE TABLE test_stats_temp AS SELECT 17;
DROP TABLE test_stats_temp;

-- Checkpoint twice: The checkpointer reports stats after reporting completion
-- of the checkpoint. But after a second checkpoint we'll see at least the
-- results of the first.
CHECKPOINT;
CHECKPOINT;

SELECT checkpoints_req > :rqst_ckpts_before FROM pg_stat_bgwriter;
SELECT wal_bytes > :wal_bytes_before FROM pg_stat_wal;


-----
-- Test that resetting stats works for reset timestamp
-----

-- Test that reset_slru with a specified SLRU works.
SELECT stats_reset AS slru_commit_ts_reset_ts FROM pg_stat_slru WHERE name = 'CommitTs' \gset
SELECT stats_reset AS slru_notify_reset_ts FROM pg_stat_slru WHERE name = 'Notify' \gset
SELECT pg_stat_reset_slru('CommitTs');
SELECT stats_reset > :'slru_commit_ts_reset_ts'::timestamptz FROM pg_stat_slru WHERE name = 'CommitTs';
SELECT stats_reset AS slru_commit_ts_reset_ts FROM pg_stat_slru WHERE name = 'CommitTs' \gset

-- Test that multiple SLRUs are reset when no specific SLRU provided to reset function
SELECT pg_stat_reset_slru(NULL);
SELECT stats_reset > :'slru_commit_ts_reset_ts'::timestamptz FROM pg_stat_slru WHERE name = 'CommitTs';
SELECT stats_reset > :'slru_notify_reset_ts'::timestamptz FROM pg_stat_slru WHERE name = 'Notify';

-- Test that reset_shared with archiver specified as the stats type works
SELECT stats_reset AS archiver_reset_ts FROM pg_stat_archiver \gset
SELECT pg_stat_reset_shared('archiver');
SELECT stats_reset > :'archiver_reset_ts'::timestamptz FROM pg_stat_archiver;
SELECT stats_reset AS archiver_reset_ts FROM pg_stat_archiver \gset

-- Test that reset_shared with bgwriter specified as the stats type works
SELECT stats_reset AS bgwriter_reset_ts FROM pg_stat_bgwriter \gset
SELECT pg_stat_reset_shared('bgwriter');
SELECT stats_reset > :'bgwriter_reset_ts'::timestamptz FROM pg_stat_bgwriter;
SELECT stats_reset AS bgwriter_reset_ts FROM pg_stat_bgwriter \gset

-- Test that reset_shared with wal specified as the stats type works
SELECT stats_reset AS wal_reset_ts FROM pg_stat_wal \gset
SELECT pg_stat_reset_shared('wal');
SELECT stats_reset > :'wal_reset_ts'::timestamptz FROM pg_stat_wal;
SELECT stats_reset AS wal_reset_ts FROM pg_stat_wal \gset

-- Test that reset_shared with no specified stats type doesn't reset anything
SELECT pg_stat_reset_shared(NULL);
SELECT stats_reset = :'archiver_reset_ts'::timestamptz FROM pg_stat_archiver;
SELECT stats_reset = :'bgwriter_reset_ts'::timestamptz FROM pg_stat_bgwriter;
SELECT stats_reset = :'wal_reset_ts'::timestamptz FROM pg_stat_wal;

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
-- db stats have objoid 0
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


-- End of Stats Test
