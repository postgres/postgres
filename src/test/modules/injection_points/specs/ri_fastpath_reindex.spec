# A foreign key check racing a rebuild of the index it resolves through.
#
# The RI fast path reads conindid before it locks the referenced table, so a
# concurrent REINDEX CONCURRENTLY can repoint the constraint and drop that
# index in between.  Cover both outcomes: an index that is already gone, and
# one that is only marked dead and so no longer receives new rows.

setup
{
    CREATE EXTENSION injection_points;
    CREATE TABLE ri_pk (id int PRIMARY KEY);
    INSERT INTO ri_pk SELECT g FROM generate_series(1, 100) g;
    CREATE TABLE ri_fk (id int PRIMARY KEY, pid int REFERENCES ri_pk(id));
    INSERT INTO ri_fk SELECT g, g FROM generate_series(1, 100) g;
    CREATE TABLE ri_old_index AS
        SELECT conindid FROM pg_constraint WHERE conname = 'ri_fk_pid_fkey';
}

teardown
{
    DROP TABLE ri_fk, ri_pk, ri_old_index;
    DROP EXTENSION injection_points;
}

# The rebuild, stopped just before it repoints the constraint.
session s1
setup
{
    SELECT injection_points_set_local();
    SELECT injection_points_attach('reindex-relation-concurrently-before-swap', 'wait');
}
# Stops it again, after the old index is dead and before it is dropped.
step park_drop
{
    SELECT injection_points_attach('reindex-relation-concurrently-before-drop', 'wait');
}
step reindex	{ REINDEX INDEX CONCURRENTLY ri_pk_pkey; }
# Forces the rebuild to have finished before anything else runs.
step reindexed	{ }

# The writer, stopped after it read conindid and before it locks ri_pk.
session s2
setup
{
    SELECT injection_points_set_local();
    SELECT injection_points_attach('ri-before-pk-lock', 'wait');
}
step upd	{ UPDATE ri_fk SET pid = 42 WHERE id = 1; }
# 500 is added while the check is parked, so only a maintained index has it.
step upd_new	{ UPDATE ri_fk SET pid = 500 WHERE id = 1; }
# Forces the check to have finished before anything else runs.
step checked	{ }

session s3
step wake_reindex
{
    SELECT injection_points_detach('reindex-relation-concurrently-before-swap');
    SELECT injection_points_wakeup('reindex-relation-concurrently-before-swap');
}
step swapped
{
    SELECT count(*) = 0 AS old_index_dropped
      FROM pg_class WHERE oid = (SELECT conindid FROM ri_old_index);
    SELECT conindid <> (SELECT conindid FROM ri_old_index) AS constraint_moved
      FROM pg_constraint WHERE conname = 'ri_fk_pid_fkey';
}
step wake_check
{
    SELECT injection_points_detach('ri-before-pk-lock');
    SELECT injection_points_wakeup('ri-before-pk-lock');
}
# Waking a session does not mean it has moved on: it can still be reported as
# waiting on the point it was woken from.  Wait for the name of the next point
# instead, which only appears once the old index is dead.  The empty step trick
# used above does not work here, since the rebuild parks again rather than
# finishing.
step await_drop
{
    DO $$
    BEGIN
        LOOP
            PERFORM 1 FROM pg_stat_activity
              WHERE wait_event = 'reindex-relation-concurrently-before-drop';
            EXIT WHEN FOUND;
            PERFORM pg_sleep(.1);
        END LOOP;
    END
    $$;
}
step dead
{
    SELECT indisvalid, indisready, indislive
      FROM pg_index WHERE indexrelid = (SELECT conindid FROM ri_old_index);
    INSERT INTO ri_pk VALUES (500);
}
step wake_drop
{
    SELECT injection_points_detach('reindex-relation-concurrently-before-drop');
    SELECT injection_points_wakeup('reindex-relation-concurrently-before-drop');
}
step rows	{ SELECT id, pid FROM ri_fk WHERE id IN (1, 2) ORDER BY id; }
# The constraint must still be enforced, not merely not crashing.
step orphan	{ INSERT INTO ri_fk VALUES (999, 12345); }

# Batched call site.
permutation reindex upd wake_reindex reindexed swapped wake_check checked rows orphan
# Dead index rather than dropped one.
permutation park_drop reindex upd_new wake_reindex await_drop dead wake_check checked wake_drop reindexed rows orphan
