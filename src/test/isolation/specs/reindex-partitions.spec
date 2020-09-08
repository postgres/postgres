# REINDEX with partitioned tables
#
# Ensure that concurrent and non-concurrent operations work correctly when
# a REINDEX is performed on a partitioned table or index.

# In the cases dealt with here, partition leaves are dropped in parallel of
# a REINDEX.  DROP TABLE gets blocked by the first transaction of REINDEX
# building the list of partitions, so it will finish executing once REINDEX
# is done.

setup
{
	CREATE TABLE reind_conc_parent (id int) PARTITION BY RANGE (id);
	CREATE TABLE reind_conc_0_10 PARTITION OF reind_conc_parent
	  FOR VALUES FROM (0) TO (10);
	CREATE TABLE reind_conc_10_20 PARTITION OF reind_conc_parent
	  FOR VALUES FROM (10) TO (20);
	INSERT INTO reind_conc_parent VALUES (generate_series(0, 19));
}

teardown
{
	DROP TABLE reind_conc_parent;
}

session "s1"
step "begin1"          { BEGIN; }
step "lockexcl1"       { LOCK reind_conc_parent IN ACCESS EXCLUSIVE MODE; }
step "lockshare1"      { LOCK reind_conc_parent IN SHARE MODE; }
step "lockupdate1"     { LOCK reind_conc_parent IN SHARE UPDATE EXCLUSIVE MODE; }
step "lockpartexcl1"   { LOCK reind_conc_10_20 IN ACCESS EXCLUSIVE MODE; }
step "lockpartshare1"  { LOCK reind_conc_10_20 IN SHARE MODE; }
step "lockpartupdate1" { LOCK reind_conc_10_20 IN SHARE UPDATE EXCLUSIVE MODE; }
step "end1"            { COMMIT; }

session "s2"
step "reindex2"        { REINDEX TABLE reind_conc_parent; }
step "reindex_conc2"   { REINDEX TABLE CONCURRENTLY reind_conc_parent; }

session "s3"
step "drop3"           { DROP TABLE reind_conc_10_20; }

# An existing partition leaf is dropped after reindex is done when the
# parent is locked.
permutation "begin1" "lockexcl1" "reindex2" "drop3" "end1"
permutation "begin1" "lockexcl1" "reindex_conc2" "drop3" "end1"
permutation "begin1" "lockshare1" "reindex2" "drop3" "end1"
permutation "begin1" "lockshare1" "reindex_conc2" "drop3" "end1"
permutation "begin1" "lockupdate1" "reindex2" "drop3" "end1"
permutation "begin1" "lockupdate1" "reindex_conc2" "drop3" "end1"

# An existing partition leaf is dropped after reindex is done when this
# leaf is locked.
permutation "begin1" "lockpartexcl1" "reindex2" "drop3" "end1"
permutation "begin1" "lockpartexcl1" "reindex_conc2" "drop3" "end1"
permutation "begin1" "lockpartshare1" "reindex2" "drop3" "end1"
permutation "begin1" "lockpartshare1" "reindex_conc2" "drop3" "end1"
permutation "begin1" "lockpartupdate1" "reindex2" "drop3" "end1"
permutation "begin1" "lockpartupdate1" "reindex_conc2" "drop3" "end1"
