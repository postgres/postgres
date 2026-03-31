# Tests that an INSERT on referencing table correctly fails when
# the referenced value disappears due to a concurrent update
setup
{
  CREATE TABLE parent (
    parent_key int PRIMARY KEY,
    aux   text NOT NULL
  );

  CREATE TABLE child (
    child_key int PRIMARY KEY,
    parent_key int8 NOT NULL REFERENCES parent
  );

  INSERT INTO parent VALUES (1, 'foo');
}

teardown
{
  DROP TABLE parent, child;
}

session s1
step s1b  { BEGIN; }
step s1i { INSERT INTO child VALUES (1, 1); }
step s1c { COMMIT; }
step s1s { SELECT * FROM child; }

session s2
step s2b  { BEGIN; }
step s2ukey { UPDATE parent SET parent_key = 2 WHERE parent_key = 1; }
step s2uaux { UPDATE parent SET aux = 'bar' WHERE parent_key = 1; }
step s2ukey2 { UPDATE parent SET parent_key = 1 WHERE parent_key = 2; }
step s2c { COMMIT; }
step s2s { SELECT * FROM parent; }

session s3
step s3b { BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s3i { INSERT INTO child VALUES (2, 1); }
step s3c { COMMIT; }
step s3s { SELECT * FROM child; }

# fail
permutation s2b s2ukey s1b s1i s2c s1c s2s s1s
# ok
permutation s2b s2uaux s1b s1i s2c s1c s2s s1s
# ok
permutation s2b s2ukey s1b s1i s2ukey2 s2c s1c s2s s1s

# RR: key update -> serialization failure
permutation s2b s2ukey s3b s3i s2c s3c s2s s3s
# RR: non-key update -> old version visible via transaction snapshot
permutation s2b s2uaux s3b s3i s2c s3c s2s s3s
