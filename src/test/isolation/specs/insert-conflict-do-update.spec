# INSERT...ON CONFLICT DO UPDATE test
#
# This test tries to expose problems with the interaction between concurrent
# sessions.

setup
{
  CREATE TABLE upsert (key int primary key, val text);
}

teardown
{
  DROP TABLE upsert;
}

session "s1"
setup
{
  BEGIN ISOLATION LEVEL READ COMMITTED;
}
step "insert1" { INSERT INTO upsert(key, val) VALUES(1, 'insert1') ON CONFLICT (key) DO UPDATE set val = upsert.val || ' updated by insert1'; }
step "c1" { COMMIT; }
step "a1" { ABORT; }

session "s2"
setup
{
  BEGIN ISOLATION LEVEL READ COMMITTED;
}
step "insert2" { INSERT INTO upsert(key, val) VALUES(1, 'insert2') ON CONFLICT (key) DO UPDATE set val = upsert.val || ' updated by insert2'; }
step "select2" { SELECT * FROM upsert; }
step "c2" { COMMIT; }
step "a2" { ABORT; }

# One session (session 2) block-waits on another (session 1) to determine if it
# should proceed with an insert or update.  Notably, this entails updating a
# tuple while there is no version of that tuple visible to the updating
# session's snapshot.  This is permitted only in READ COMMITTED mode.
permutation "insert1" "insert2" "c1" "select2" "c2"
permutation "insert1" "insert2" "a1" "select2" "c2"
