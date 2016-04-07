# Read-write-unique test.
# From bug report 9301.

setup
{
  CREATE TABLE test (
    key   integer UNIQUE,
    val   text
  );

  CREATE OR REPLACE FUNCTION insert_unique(k integer, v text) RETURNS void
  LANGUAGE SQL AS $$
    INSERT INTO test (key, val) SELECT k, v WHERE NOT EXISTS (SELECT key FROM test WHERE key = k);
  $$;
}

teardown
{
  DROP FUNCTION insert_unique(integer, text);
  DROP TABLE test;
}

session "s1"
setup { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step "rw1" { SELECT insert_unique(1, '1'); }
step "c1" { COMMIT; }

session "s2"
setup { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step "rw2" { SELECT insert_unique(1, '2'); }
step "c2" { COMMIT; }

permutation "rw1" "rw2" "c1" "c2"
