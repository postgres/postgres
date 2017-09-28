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

session "s1"
setup				{ BEGIN; }
step "s1_update"	{ UPDATE tab_freeze SET x = x + 1 WHERE id = 3; }
step "s1_commit"	{ COMMIT; }
step "s1_vacuum"	{ VACUUM FREEZE tab_freeze; }

session "s2"
setup				{ BEGIN; }
step "s2_key_share"	{ SELECT id FROM tab_freeze WHERE id = 3 FOR KEY SHARE; }
step "s2_commit"	{ COMMIT; }
