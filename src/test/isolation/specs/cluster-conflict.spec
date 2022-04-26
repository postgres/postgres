# Tests for locking conflicts with CLUSTER command.

setup
{
	CREATE ROLE regress_cluster_conflict;
	CREATE TABLE cluster_tab (a int);
	CREATE INDEX cluster_ind ON cluster_tab(a);
	ALTER TABLE cluster_tab OWNER TO regress_cluster_conflict;
}

teardown
{
	DROP TABLE cluster_tab;
	DROP ROLE regress_cluster_conflict;
}

session s1
step s1_begin          { BEGIN; }
step s1_lock           { LOCK cluster_tab IN SHARE UPDATE EXCLUSIVE MODE; }
step s1_commit         { COMMIT; }

session s2
step s2_auth           { SET ROLE regress_cluster_conflict; }
step s2_cluster        { CLUSTER cluster_tab USING cluster_ind; }
step s2_reset          { RESET ROLE; }

# The role has privileges to cluster the table, CLUSTER will block if
# another session holds a lock on the table and succeed in all cases.
permutation s1_begin s1_lock s2_auth s2_cluster s1_commit s2_reset
permutation s1_begin s2_auth s1_lock s2_cluster s1_commit s2_reset
