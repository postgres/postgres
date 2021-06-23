# Test for interactions of tuple freezing with dead, as well as recently-dead
# tuples using multixacts via FOR KEY SHARE.
setup
{
  CREATE TABLE tab_freeze (
    id int PRIMARY KEY,
    name char(3),
    x int);
  INSERT INTO tab_freeze VALUES (1, '111', 0);
  INSERT INTO tab_freeze VALUES (3, '333', 0);
}

teardown
{
  DROP TABLE tab_freeze;
}

session s1
step s1_begin		{ BEGIN; }
step s1_update		{ UPDATE tab_freeze SET x = x + 1 WHERE id = 3; }
step s1_commit		{ COMMIT; }
step s1_selectone	{
    BEGIN;
    SET LOCAL enable_seqscan = false;
    SET LOCAL enable_bitmapscan = false;
    SELECT * FROM tab_freeze WHERE id = 3;
    COMMIT;
}
step s1_selectall	{ SELECT * FROM tab_freeze ORDER BY name, id; }

session s2
step s2_begin		{ BEGIN; }
step s2_key_share	{ SELECT id FROM tab_freeze WHERE id = 3 FOR KEY SHARE; }
step s2_commit		{ COMMIT; }
step s2_vacuum		{ VACUUM FREEZE tab_freeze; }

session s3
step s3_begin		{ BEGIN; }
step s3_key_share	{ SELECT id FROM tab_freeze WHERE id = 3 FOR KEY SHARE; }
step s3_commit		{ COMMIT; }

# This permutation verifies that a previous bug
#     https://postgr.es/m/E5711E62-8FDF-4DCA-A888-C200BF6B5742@amazon.com
#     https://postgr.es/m/20171102112019.33wb7g5wp4zpjelu@alap3.anarazel.de
# is not reintroduced. We used to make wrong pruning / freezing
# decision for multixacts, which could lead to a) broken hot chains b)
# dead rows being revived.
permutation s1_begin s2_begin s3_begin # start transactions
   s1_update s2_key_share s3_key_share # have xmax be a multi with an updater, updater being oldest xid
   s1_update # create additional row version that has multis
   s1_commit s2_commit # commit both updater and share locker
   s2_vacuum # due to bug in freezing logic, we used to *not* prune updated row, and then froze it
   s1_selectone # if hot chain is broken, the row can't be found via index scan
   s3_commit # commit remaining open xact
   s2_vacuum # pruning / freezing in broken hot chains would unset xmax, reviving rows
   s1_selectall # show borkedness
