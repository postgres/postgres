# Test that pruning and vacuuming pay attention to concurrent sessions
# in the right way. For normal relations that means that rows cannot
# be pruned away if there's an older snapshot, in contrast to that
# temporary tables should nearly always be prunable.
#
# NB: Think hard before adding a test showing that rows in permanent
# tables get pruned - it's quite likely that it'd be racy, e.g. due to
# an autovacuum worker holding a snapshot.

setup {
    CREATE OR REPLACE FUNCTION explain_json(p_query text)
    RETURNS json
    LANGUAGE plpgsql AS $$
        DECLARE
            v_ret json;
        BEGIN
            EXECUTE p_query INTO STRICT v_ret;
            RETURN v_ret;
        END;$$;
}

teardown {
    DROP FUNCTION explain_json(text);
}

session lifeline

# Start a transaction, force a snapshot to be held
step ll_start
{
    BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ;
    SELECT 1;
}

step ll_commit { COMMIT; }


session pruner

setup
{
    SET enable_seqscan = false;
    SET enable_indexscan = false;
    SET enable_bitmapscan = false;
}

step pruner_create_temp
{
    CREATE TEMPORARY TABLE horizons_tst (data int unique) WITH (autovacuum_enabled = off);
    INSERT INTO horizons_tst(data) VALUES(1),(2);
}

step pruner_create_perm
{
    CREATE TABLE horizons_tst (data int unique) WITH (autovacuum_enabled = off);
    INSERT INTO horizons_tst(data) VALUES(1),(2);
}

# Temp tables cannot be dropped in the teardown, so just always do so
# as part of the permutation
step pruner_drop
{
    DROP TABLE horizons_tst;
}

step pruner_delete
{
    DELETE FROM horizons_tst;
}

step pruner_begin { BEGIN; }
step pruner_commit { COMMIT; }

step pruner_vacuum
{
    VACUUM horizons_tst;
}

# Show the heap fetches of an ordered index-only-scan (other plans
# have been forbidden above) - that tells us how many non-killed leaf
# entries there are.
step pruner_query
{
    SELECT explain_json($$
        EXPLAIN (FORMAT json, BUFFERS, ANALYZE)
	  SELECT * FROM horizons_tst ORDER BY data;$$)->0->'Plan'->'Heap Fetches';
}

# Verify that the query plan still is an IOS
step pruner_query_plan
{
    EXPLAIN (COSTS OFF) SELECT * FROM horizons_tst ORDER BY data;
}


# Show that with a permanent relation deleted rows cannot be pruned
# away if there's a concurrent session still seeing the rows.
permutation
    pruner_create_perm
    ll_start
    pruner_query_plan
    # Run query that could do pruning twice, first has chance to prune,
    # second would not perform heap fetches if first query did.
    pruner_query
    pruner_query
    pruner_delete
    pruner_query
    pruner_query
    ll_commit
    pruner_drop

# Show that with a temporary relation deleted rows can be pruned away,
# even if there's a concurrent session with a snapshot from before the
# deletion. That's safe because the session with the older snapshot
# cannot access the temporary table.
permutation
    pruner_create_temp
    ll_start
    pruner_query_plan
    pruner_query
    pruner_query
    pruner_delete
    pruner_query
    pruner_query
    ll_commit
    pruner_drop

# Verify that pruning in temporary relations doesn't remove rows still
# visible in the current session
permutation
    pruner_create_temp
    ll_start
    pruner_query
    pruner_query
    pruner_begin
    pruner_delete
    pruner_query
    pruner_query
    ll_commit
    pruner_commit
    pruner_drop

# Show that vacuum cannot remove deleted rows still visible to another
# session's snapshot, when accessing a permanent table.
permutation
    pruner_create_perm
    ll_start
    pruner_query
    pruner_query
    pruner_delete
    pruner_vacuum
    pruner_query
    pruner_query
    ll_commit
    pruner_drop

# Show that vacuum can remove deleted rows still visible to another
# session's snapshot, when accessing a temporary table.
permutation
    pruner_create_temp
    ll_start
    pruner_query
    pruner_query
    pruner_delete
    pruner_vacuum
    pruner_query
    pruner_query
    ll_commit
    pruner_drop
