# Tests for inheritance trees with temporary relations
#
# Inheritance trees are allowed to mix relations with different persistence
# as long as a persistent child relation does not try to inherit from a
# temporary parent.  This checks several scenarios with SELECT, INSERT, UPDATE,
# DELETE and TRUNCATE.  Any temporary relation inheriting from the same
# persistent parent should be isolated and handled only in its own session.

setup
{
  CREATE TABLE inh_parent (a int);
}

teardown
{
  DROP TABLE inh_parent;
}

# Session 1 executes actions which act directly on both the parent and
# its child.  Abbreviation "c" is used for queries working on the child
# and "p" on the parent.
session "s1"
setup
{
  CREATE TEMPORARY TABLE inh_temp_child_s1 () INHERITS (inh_parent);
}
step "s1_begin" { BEGIN; }
step "s1_truncate_p" { TRUNCATE inh_parent; }
step "s1_select_p" { SELECT a FROM inh_parent; }
step "s1_select_c" { SELECT a FROM inh_temp_child_s1; }
step "s1_insert_p" { INSERT INTO inh_parent VALUES (1), (2); }
step "s1_insert_c" { INSERT INTO inh_temp_child_s1 VALUES (3), (4); }
step "s1_update_p" { UPDATE inh_parent SET a = 11 WHERE a = 1; }
step "s1_update_c" { UPDATE inh_parent SET a = 13 WHERE a IN (3, 5); }
step "s1_delete_p" { DELETE FROM inh_parent WHERE a = 2; }
step "s1_delete_c" { DELETE FROM inh_parent WHERE a IN (4, 6); }
step "s1_commit" { COMMIT; }
teardown
{
  DROP TABLE inh_temp_child_s1;
}

# Session 2 executes actions on the parent which act only on the child.
session "s2"
setup
{
  CREATE TEMPORARY TABLE inh_temp_child_s2 () INHERITS (inh_parent);
}
step "s2_truncate_p" { TRUNCATE inh_parent; }
step "s2_select_p" { SELECT a FROM inh_parent; }
step "s2_select_c" { SELECT a FROM inh_temp_child_s2; }
step "s2_insert_c" { INSERT INTO inh_temp_child_s2 VALUES (5), (6); }
step "s2_update_c" { UPDATE inh_parent SET a = 15 WHERE a IN (3, 5); }
step "s2_delete_c" { DELETE FROM inh_parent WHERE a IN (4, 6); }
teardown
{
  DROP TABLE inh_temp_child_s2;
}

# Check INSERT behavior across sessions
permutation "s1_insert_p" "s1_insert_c" "s2_insert_c" "s1_select_p" "s1_select_c" "s2_select_p" "s2_select_c"

# Check UPDATE behavior across sessions
permutation "s1_insert_p" "s1_insert_c" "s2_insert_c" "s1_update_p" "s1_update_c" "s1_select_p" "s1_select_c" "s2_select_p" "s2_select_c"
permutation "s1_insert_p" "s1_insert_c" "s2_insert_c" "s2_update_c" "s1_select_p" "s1_select_c" "s2_select_p" "s2_select_c"

# Check DELETE behavior across sessions
permutation "s1_insert_p" "s1_insert_c" "s2_insert_c" "s1_delete_p" "s1_delete_c" "s1_select_p" "s1_select_c" "s2_select_p" "s2_select_c"
permutation "s1_insert_p" "s1_insert_c" "s2_insert_c" "s2_delete_c" "s1_select_p" "s1_select_c" "s2_select_p" "s2_select_c"

# Check TRUNCATE behavior across sessions
permutation "s1_insert_p" "s1_insert_c" "s2_insert_c" "s1_truncate_p" "s1_select_p" "s1_select_c" "s2_select_p" "s2_select_c"
permutation "s1_insert_p" "s1_insert_c" "s2_insert_c" "s2_truncate_p" "s1_select_p" "s1_select_c" "s2_select_p" "s2_select_c"

# TRUNCATE on a parent tree does not block access to temporary child relation
# of another session, and blocks when scanning the parent.
permutation "s1_insert_p" "s1_insert_c" "s2_insert_c" "s1_begin" "s1_truncate_p" "s2_select_p" "s1_commit"
permutation "s1_insert_p" "s1_insert_c" "s2_insert_c" "s1_begin" "s1_truncate_p" "s2_select_c" "s1_commit"
