# Tests for locking conflicts with TRUNCATE commands.

setup
{
	CREATE ROLE regress_truncate_conflict;
	CREATE TABLE truncate_tab (a int);
}

teardown
{
	DROP TABLE truncate_tab;
	DROP ROLE regress_truncate_conflict;
}

session "s1"
step "s1_begin"          { BEGIN; }
step "s1_tab_lookup"     { SELECT count(*) >= 0 FROM truncate_tab; }
step "s1_commit"         { COMMIT; }

session "s2"
step "s2_grant"          { GRANT TRUNCATE ON truncate_tab TO regress_truncate_conflict; }
step "s2_auth"           { SET ROLE regress_truncate_conflict; }
step "s2_truncate"       { TRUNCATE truncate_tab; }
step "s2_reset"          { RESET ROLE; }

# The role doesn't have privileges to truncate the table, so TRUNCATE should
# immediately fail without waiting for a lock.
permutation "s1_begin" "s1_tab_lookup" "s2_auth" "s2_truncate" "s1_commit" "s2_reset"
permutation "s1_begin" "s2_auth" "s2_truncate" "s1_tab_lookup" "s1_commit" "s2_reset"
permutation "s1_begin" "s2_auth" "s1_tab_lookup" "s2_truncate" "s1_commit" "s2_reset"
permutation "s2_auth" "s2_truncate" "s1_begin" "s1_tab_lookup" "s1_commit" "s2_reset"

# The role has privileges to truncate the table, TRUNCATE will block if
# another session holds a lock on the table and succeed in all cases.
permutation "s1_begin" "s1_tab_lookup" "s2_grant" "s2_auth" "s2_truncate" "s1_commit" "s2_reset"
permutation "s1_begin" "s2_grant" "s2_auth" "s2_truncate" "s1_tab_lookup" "s1_commit" "s2_reset"
permutation "s1_begin" "s2_grant" "s2_auth" "s1_tab_lookup" "s2_truncate" "s1_commit" "s2_reset"
permutation "s2_grant" "s2_auth" "s2_truncate" "s1_begin" "s1_tab_lookup" "s1_commit" "s2_reset"
