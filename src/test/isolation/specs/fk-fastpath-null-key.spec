# A foreign key may reference a nullable UNIQUE column, not only a NOT NULL
# primary key.  Test that the RI fast-path check copes when a concurrent
# transaction sets the referenced key to NULL.
#
# s2's INSERT probes the PK index, finds the (u=5) row, and blocks on s1's
# in-progress key-changing UPDATE.  After s1 commits, the tuple lock follows
# the update chain (table_tuple_lock with TUPLE_LOCK_FLAG_FIND_LAST_VERSION)
# to the now-NULL version.  The check must treat that as "referenced row not
# found" and raise an ordinary foreign-key violation -- not assume that a
# referenced key can never be NULL.
#
# Two permutations exercise the two fast-path flush routines: a single-row
# INSERT goes through ri_FastPathFlushLoop()/recheck_matched_pk_tuple(), while
# a multi-row single-column INSERT goes through ri_FastPathFlushArray().

setup
{
  CREATE TABLE pktable (u int UNIQUE, c int);
  CREATE TABLE fktable (a int REFERENCES pktable (u));
  INSERT INTO pktable VALUES (5, 1), (6, 2);
}

teardown
{
  DROP TABLE fktable, pktable;
}

session s1
step s1b		{ BEGIN; }
step s1upd_null	{ UPDATE pktable SET u = NULL WHERE u = 5; }
step s1c		{ COMMIT; }

session s2
# single-row batch -> per-row loop flush path
step s2ins		{ INSERT INTO fktable VALUES (5); }
# multi-row single-column batch -> SK_SEARCHARRAY flush path
step s2ins_arr	{ INSERT INTO fktable VALUES (5), (6); }

permutation s1b s1upd_null s2ins s1c
permutation s1b s1upd_null s2ins_arr s1c
