# REINDEX CONCURRENTLY of a TOAST index racing a TOAST insert.
#
# s2 holds the TOAST indexes open while the new index becomes ready, then
# inserts again before the swap.
setup
{
    CREATE EXTENSION injection_points;
    CREATE TABLE reind_toast (id int, val text);
    ALTER TABLE reind_toast ALTER COLUMN val SET STORAGE EXTERNAL;
    INSERT INTO reind_toast VALUES (1, repeat('x', 10000));
}

teardown
{
    DROP TABLE reind_toast;
    DROP EXTENSION injection_points;
}

session s1
step reindex	{ REINDEX TABLE CONCURRENTLY reind_toast; }
# Empty step, to force the REINDEX to finish.
step reindexed	{ }

# Writer, stopped with the TOAST indexes open.
session s2
setup
{
    SELECT injection_points_set_local();
    SELECT injection_points_attach('toast-save-datum-indexes-opened', 'wait');
}
# TOAST indexes opened, new index is not ready yet.
step ins1	{ INSERT INTO reind_toast VALUES (2, repeat('y', 10000)); }
# Empty step, to force ins1 to finish.
step inserted	{ }
# Runs once new index is validated, before swapping.
step ins2	{ INSERT INTO reind_toast VALUES (3, repeat('z', 10000)); }
# left() always detoasts the value.
step read	{ SELECT id, left(val, 1) FROM reind_toast ORDER BY id; }

session s3
# Holds the REINDEX back until ins1 has opened the TOAST indexes.
step lock	{ BEGIN; LOCK TABLE reind_toast IN ROW EXCLUSIVE MODE; }
step unlock	{ COMMIT; }
# Keeps the REINDEX waiting for old snapshots after it validates the new
# index.
step snapshot	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1; }
# Resumes ins1.
step wake
{
    SELECT injection_points_detach('toast-save-datum-indexes-opened');
    SELECT injection_points_wakeup('toast-save-datum-indexes-opened');
}
step release	{ COMMIT; }

permutation lock reindex ins1 unlock snapshot wake inserted ins2 release reindexed read
