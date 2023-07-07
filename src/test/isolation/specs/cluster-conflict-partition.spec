# Tests for locking conflicts with CLUSTER command and partitions.

setup
{
	CREATE ROLE regress_cluster_part;
	CREATE TABLE cluster_part_tab (a int) PARTITION BY LIST (a);
	CREATE TABLE cluster_part_tab1 PARTITION OF cluster_part_tab FOR VALUES IN (1);
	CREATE TABLE cluster_part_tab2 PARTITION OF cluster_part_tab FOR VALUES IN (2);
	CREATE INDEX cluster_part_ind ON cluster_part_tab(a);
	ALTER TABLE cluster_part_tab OWNER TO regress_cluster_part;
}

teardown
{
	DROP TABLE cluster_part_tab;
	DROP ROLE regress_cluster_part;
}

session s1
step s1_begin          { BEGIN; }
step s1_lock_parent    { LOCK cluster_part_tab IN SHARE UPDATE EXCLUSIVE MODE; }
step s1_lock_child     { LOCK cluster_part_tab1 IN SHARE UPDATE EXCLUSIVE MODE; }
step s1_commit         { COMMIT; }

session s2
step s2_auth           { SET ROLE regress_cluster_part; }
step s2_cluster        { CLUSTER cluster_part_tab USING cluster_part_ind; }
step s2_reset          { RESET ROLE; }

# CLUSTER on the parent waits if locked, passes for all cases.
permutation s1_begin s1_lock_parent s2_auth s2_cluster s1_commit s2_reset
permutation s1_begin s2_auth s1_lock_parent s2_cluster s1_commit s2_reset

# When taking a lock on a partition leaf, CLUSTER on the parent skips
# the leaf, passes for all cases.
permutation s1_begin s1_lock_child s2_auth s2_cluster s1_commit s2_reset
permutation s1_begin s2_auth s1_lock_child s2_cluster s1_commit s2_reset
