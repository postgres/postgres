# This test is like sto_using_select, except that we test access via a
# hash index.

setup
{
    CREATE TABLE sto1 (c int NOT NULL);
    INSERT INTO sto1 SELECT generate_series(1, 1000);
    CREATE INDEX idx_sto1 ON sto1 USING HASH (c);
}
setup
{
    VACUUM ANALYZE sto1;
}

teardown
{
    DROP TABLE sto1;
}

session "s1"
setup			{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step "noseq"	{ SET enable_seqscan = false; }
step "s1f1"		{ SELECT c FROM sto1 where c = 1000; }
step "s1f2"		{ SELECT c FROM sto1 where c = 1001; }
teardown		{ ROLLBACK; }

session "s2"
step "s2sleep"	{ SELECT setting, pg_sleep(6) FROM pg_settings WHERE name = 'old_snapshot_threshold'; }
step "s2u"		{ UPDATE sto1 SET c = 1001 WHERE c = 1000; }

permutation "noseq" "s1f1" "s2sleep" "s2u" "s1f2"
