# Test decoding of in-progress transaction containing dml and a concurrent
# transaction with ddl operation. The transaction containing ddl operation
# should not get streamed as it doesn't have any changes.

setup
{
  SELECT 'init' FROM pg_create_logical_replication_slot('isolation_slot', 'test_decoding');

  -- consume DDL
  SELECT data FROM pg_logical_slot_get_changes('isolation_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
  CREATE OR REPLACE FUNCTION large_val() RETURNS TEXT LANGUAGE SQL AS 'select array_agg(md5(g::text))::text from generate_series(1, 80000) g';
}

teardown
{
    DROP TABLE IF EXISTS stream_test;
    DROP TABLE IF EXISTS stream_test1;
    SELECT 'stop' FROM pg_drop_replication_slot('isolation_slot');
}

session "s0"
setup { SET synchronous_commit=on; }
step "s0_begin" { BEGIN; }
step "s0_ddl"   {CREATE TABLE stream_test1(data text);}

session "s2"
setup { SET synchronous_commit=on; }
step "s2_ddl"   {CREATE TABLE stream_test2(data text);}

# The transaction commit for s1_ddl will add the INTERNAL_SNAPSHOT change to
# the currently running s0_ddl and we want to test that s0_ddl should not get
# streamed when user asked to skip-empty-xacts. Similarly, the
# INTERNAL_SNAPSHOT change added by s2_ddl should not change the results for
# what gets streamed.
session "s1"
setup { SET synchronous_commit=on; }
step "s1_ddl"   { CREATE TABLE stream_test(data text); }
step "s1_begin" { BEGIN; }
step "s1_toast_insert" {INSERT INTO stream_test SELECT large_val();}
step "s1_commit" { COMMIT; }
step "s1_get_stream_changes" { SELECT data FROM pg_logical_slot_get_changes('isolation_slot', NULL,NULL, 'include-xids', '0', 'skip-empty-xacts', '1', 'stream-changes', '1');}

permutation "s0_begin" "s0_ddl" "s1_ddl" "s1_begin" "s1_toast_insert" "s2_ddl" "s1_commit" "s1_get_stream_changes"
