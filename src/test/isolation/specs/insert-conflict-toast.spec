# INSERT...ON CONFLICT test on table with TOAST
#
# This test verifies that speculatively inserted toast rows do not
# cause conflicts. It does so by using expression index over a
# function which acquires an advisory lock, triggering two index
# insertions to happen almost at the same time. This is not guaranteed
# to lead to a failed speculative insertion, but makes one quite
# likely.

setup
{
  CREATE TABLE ctoast (key int primary key, val text);
  CREATE OR REPLACE FUNCTION ctoast_lock_func(int) RETURNS INT IMMUTABLE LANGUAGE SQL AS 'select pg_advisory_xact_lock_shared(1); select $1;';
  CREATE OR REPLACE FUNCTION ctoast_large_val() RETURNS TEXT LANGUAGE SQL AS 'select array_agg(md5(g::text))::text from generate_series(1, 256) g';
  CREATE UNIQUE INDEX ctoast_lock_idx ON ctoast (ctoast_lock_func(key));
}

teardown
{
  DROP TABLE ctoast;
  DROP FUNCTION ctoast_lock_func(int);
  DROP FUNCTION ctoast_large_val();
}

session "s1"
setup
{
  BEGIN ISOLATION LEVEL READ COMMITTED;
  SELECT pg_advisory_xact_lock(1);
}
step "s1commit" { COMMIT; }

session "s2"
setup
{
  SET default_transaction_isolation = 'read committed';
}
step "s2insert" {
  INSERT INTO ctoast (key, val) VALUES (1, ctoast_large_val()) ON CONFLICT DO NOTHING;
}

session "s3"
setup
{
  SET default_transaction_isolation = 'read committed';
}
step "s3insert" {
  INSERT INTO ctoast (key, val) VALUES (1, ctoast_large_val()) ON CONFLICT DO NOTHING;
}

permutation "s2insert" "s3insert" "s1commit"
