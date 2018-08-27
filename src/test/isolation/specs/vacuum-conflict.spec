# Tests for locking conflicts with VACUUM and ANALYZE commands.

setup
{
	CREATE ROLE regress_vacuum_conflict;
	CREATE TABLE vacuum_tab (a int);
}

teardown
{
	DROP TABLE vacuum_tab;
	DROP ROLE regress_vacuum_conflict;
}

session "s1"
step "s1_begin"          { BEGIN; }
step "s1_lock"           { LOCK vacuum_tab IN SHARE UPDATE EXCLUSIVE MODE; }
step "s1_commit"         { COMMIT; }

session "s2"
step "s2_grant"          { ALTER TABLE vacuum_tab OWNER TO regress_vacuum_conflict; }
step "s2_auth"           { SET ROLE regress_vacuum_conflict; }
step "s2_vacuum"         { VACUUM vacuum_tab; }
step "s2_analyze"        { ANALYZE vacuum_tab; }
step "s2_reset"          { RESET ROLE; }

# The role doesn't have privileges to vacuum the table, so VACUUM should
# immediately skip the table without waiting for a lock.
permutation "s1_begin" "s1_lock" "s2_auth" "s2_vacuum" "s1_commit" "s2_reset"
permutation "s1_begin" "s2_auth" "s2_vacuum" "s1_lock" "s1_commit" "s2_reset"
permutation "s1_begin" "s2_auth" "s1_lock" "s2_vacuum" "s1_commit" "s2_reset"
permutation "s2_auth" "s2_vacuum" "s1_begin" "s1_lock" "s1_commit" "s2_reset"

# Same as previously for ANALYZE
permutation "s1_begin" "s1_lock" "s2_auth" "s2_analyze" "s1_commit" "s2_reset"
permutation "s1_begin" "s2_auth" "s2_analyze" "s1_lock" "s1_commit" "s2_reset"
permutation "s1_begin" "s2_auth" "s1_lock" "s2_analyze" "s1_commit" "s2_reset"
permutation "s2_auth" "s2_analyze" "s1_begin" "s1_lock" "s1_commit" "s2_reset"

# The role has privileges to vacuum the table, VACUUM will block if
# another session holds a lock on the table and succeed in all cases.
permutation "s1_begin" "s2_grant" "s1_lock" "s2_auth" "s2_vacuum" "s1_commit" "s2_reset"
permutation "s1_begin" "s2_grant" "s2_auth" "s2_vacuum" "s1_lock" "s1_commit" "s2_reset"
permutation "s1_begin" "s2_grant" "s2_auth" "s1_lock" "s2_vacuum" "s1_commit" "s2_reset"
permutation "s2_grant" "s2_auth" "s2_vacuum" "s1_begin" "s1_lock" "s1_commit" "s2_reset"

# Same as previously for ANALYZE
permutation "s1_begin" "s2_grant" "s1_lock" "s2_auth" "s2_analyze" "s1_commit" "s2_reset"
permutation "s1_begin" "s2_grant" "s2_auth" "s2_analyze" "s1_lock" "s1_commit" "s2_reset"
permutation "s1_begin" "s2_grant" "s2_auth" "s1_lock" "s2_analyze" "s1_commit" "s2_reset"
permutation "s2_grant" "s2_auth" "s2_analyze" "s1_begin" "s1_lock" "s1_commit" "s2_reset"
