# INSERT...ON CONFLICT DO SELECT test
#
# This test verifies locking behavior of ON CONFLICT DO SELECT with different
# lock strengths: no lock, FOR KEY SHARE, FOR SHARE, FOR NO KEY UPDATE, and
# FOR UPDATE.

setup
{
  CREATE TABLE doselect (key int primary key, val text);
  INSERT INTO doselect VALUES (1, 'original');
}

teardown
{
  DROP TABLE doselect;
}

session s1
setup
{
  BEGIN ISOLATION LEVEL READ COMMITTED;
}
step insert1 { INSERT INTO doselect(key, val) VALUES(1, 'insert1') ON CONFLICT (key) DO SELECT RETURNING *; }
step insert1_keyshare { INSERT INTO doselect(key, val) VALUES(1, 'insert1') ON CONFLICT (key) DO SELECT FOR KEY SHARE RETURNING *; }
step insert1_share { INSERT INTO doselect(key, val) VALUES(1, 'insert1') ON CONFLICT (key) DO SELECT FOR SHARE RETURNING *; }
step insert1_nokeyupd { INSERT INTO doselect(key, val) VALUES(1, 'insert1') ON CONFLICT (key) DO SELECT FOR NO KEY UPDATE RETURNING *; }
step insert1_update { INSERT INTO doselect(key, val) VALUES(1, 'insert1') ON CONFLICT (key) DO SELECT FOR UPDATE RETURNING *; }
step c1 { COMMIT; }
step a1 { ABORT; }

session s2
setup
{
  BEGIN ISOLATION LEVEL READ COMMITTED;
}
step insert2 { INSERT INTO doselect(key, val) VALUES(1, 'insert2') ON CONFLICT (key) DO SELECT RETURNING *; }
step insert2_update { INSERT INTO doselect(key, val) VALUES(1, 'insert2') ON CONFLICT (key) DO SELECT FOR UPDATE RETURNING *; }
step select2 { SELECT * FROM doselect; }
step c2 { COMMIT; }

# Test 1: DO SELECT without locking - should not block
permutation insert1 insert2 c1 select2 c2

# Test 2: DO SELECT FOR UPDATE - should block until first transaction commits
permutation insert1_update insert2_update c1 select2 c2

# Test 3: DO SELECT FOR UPDATE - should unblock when first transaction aborts
permutation insert1_update insert2_update a1 select2 c2

# Test 4: Different lock strengths all properly acquire locks
permutation insert1_keyshare insert2_update c1 select2 c2
permutation insert1_share insert2_update c1 select2 c2
permutation insert1_nokeyupd insert2_update c1 select2 c2
