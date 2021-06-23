# Test locking of a tuple with a committed key-update.  In this case,
# the update conflicts with the lock, so failures are expected, except
# in READ COMMITTED isolation mode.
#
# Some of the permutations are commented out that work fine in the
# lock-committed-update test, because in this case the update blocks.

setup
{
    DROP TABLE IF EXISTS lcku_table;
    CREATE TABLE lcku_table (id INTEGER, value TEXT, PRIMARY KEY (id) INCLUDE (value));
    INSERT INTO lcku_table VALUES (1, 'one');
    INSERT INTO lcku_table VALUES (3, 'two');
}

teardown
{
    DROP TABLE lcku_table;
}

session s1
step s1b      { BEGIN; }
step s1l      { SELECT pg_advisory_lock(578902068); }
step s1u      { UPDATE lcku_table SET id = 2 WHERE id = 3; }
step s1hint   { SELECT * FROM lcku_table; }
step s1ul     { SELECT pg_advisory_unlock(578902068); }
step s1c      { COMMIT; }
teardown      { SELECT pg_advisory_unlock_all(); }

session s2
step s2b1     { BEGIN ISOLATION LEVEL READ COMMITTED; }
step s2b2     { BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s2b3     { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2l      { SELECT * FROM lcku_table WHERE pg_advisory_lock(578902068) IS NOT NULL FOR KEY SHARE; }
step s2c      { COMMIT; }
teardown      { SELECT pg_advisory_unlock_all(); }

permutation s1b s2b1 s1l s2l        s1u s1c          s1ul s2c
permutation s1b s2b1 s1l s1u        s2l s1c          s1ul s2c
#permutation s1b s2b1 s1l s2l s1ul s1u s1c                 s2c
permutation s1b s2b1 s1l s1u s1ul s2l s1c                 s2c

permutation s1b s2b1 s1l s2l        s1u s1c s1hint s1ul s2c
permutation s1b s2b1 s1l s1u        s2l s1c s1hint s1ul s2c
#permutation s1b s2b1 s1l s2l s1ul s1u s1c s1hint        s2c
permutation s1b s2b1 s1l s1u s1ul s2l s1c s1hint        s2c

permutation s1b s2b2 s1l s2l        s1u s1c          s1ul s2c
permutation s1b s2b2 s1l s1u        s2l s1c          s1ul s2c
#permutation s1b s2b2 s1l s2l s1ul s1u s1c                 s2c
permutation s1b s2b2 s1l s1u s1ul s2l s1c                 s2c

permutation s1b s2b2 s1l s2l        s1u s1c s1hint s1ul s2c
permutation s1b s2b2 s1l s1u        s2l s1c s1hint s1ul s2c
#permutation s1b s2b2 s1l s2l s1ul s1u s1c s1hint        s2c
permutation s1b s2b2 s1l s1u s1ul s2l s1c s1hint        s2c

permutation s1b s2b3 s1l s2l        s1u s1c          s1ul s2c
permutation s1b s2b3 s1l s1u        s2l s1c          s1ul s2c
#permutation s1b s2b3 s1l s2l s1ul s1u s1c                 s2c
permutation s1b s2b3 s1l s1u s1ul s2l s1c                 s2c

permutation s1b s2b3 s1l s2l        s1u s1c s1hint s1ul s2c
permutation s1b s2b3 s1l s1u        s2l s1c s1hint s1ul s2c
#permutation s1b s2b3 s1l s2l s1ul s1u s1c s1hint        s2c
permutation s1b s2b3 s1l s1u s1ul s2l s1c s1hint        s2c
