# INSERT...ON CONFLICT DO UPDATE test
#
# This test shows a plausible scenario in which the user might wish to UPDATE a
# value that is also constrained by the unique index that is the arbiter of
# whether the alternative path should be taken.

setup
{
  CREATE TABLE upsert (key text not null, payload text);
  CREATE UNIQUE INDEX ON upsert(lower(key));
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
step "insert1" { INSERT INTO upsert(key, payload) VALUES('FooFoo', 'insert1') ON CONFLICT (lower(key)) DO UPDATE set key = EXCLUDED.key, payload = upsert.payload || ' updated by insert1'; }
step "c1" { COMMIT; }
step "a1" { ABORT; }

session "s2"
setup
{
  BEGIN ISOLATION LEVEL READ COMMITTED;
}
step "insert2" { INSERT INTO upsert(key, payload) VALUES('FOOFOO', 'insert2') ON CONFLICT (lower(key)) DO UPDATE set key = EXCLUDED.key, payload = upsert.payload || ' updated by insert2'; }
step "select2" { SELECT * FROM upsert; }
step "c2" { COMMIT; }
step "a2" { ABORT; }

# One session (session 2) block-waits on another (session 1) to determine if it
# should proceed with an insert or update.  The user can still usefully UPDATE
# a column constrained by a unique index, as the example illustrates.
permutation "insert1" "insert2" "c1" "select2" "c2"
permutation "insert1" "insert2" "a1" "select2" "c2"
