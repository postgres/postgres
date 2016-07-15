# Test locking of a tuple with a committed update.  When the lock does not
# conflict with the update, no blocking and no serializability errors should
# occur.

setup
{
    DROP TABLE IF EXISTS lcu_table;
    CREATE TABLE lcu_table (id INTEGER PRIMARY KEY, value TEXT);
    INSERT INTO lcu_table VALUES (1, 'one');
}

teardown
{
    DROP TABLE lcu_table;
}

session "s1"
step "s1b"    { BEGIN; }
step "s1l"    { SELECT pg_advisory_lock(380170116); }
step "s1u"    { UPDATE lcu_table SET value = 'two' WHERE id = 1; }
step "s1hint" { SELECT * FROM lcu_table; }
step "s1ul"   { SELECT pg_advisory_unlock(380170116); }
step "s1c"    { COMMIT; }
teardown      { SELECT pg_advisory_unlock_all(); }

session "s2"
step "s2b1"    { BEGIN ISOLATION LEVEL READ COMMITTED; }
step "s2b2"    { BEGIN ISOLATION LEVEL REPEATABLE READ; }
step "s2b3"    { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step "s2l"    { SELECT * FROM lcu_table WHERE pg_advisory_lock(380170116) IS NOT NULL FOR KEY SHARE; }
step "s2c"    { COMMIT; }
teardown      { SELECT pg_advisory_unlock_all(); }

permutation "s1b" "s2b1" "s1l" "s2l"        "s1u" "s1c"          "s1ul" "s2c"
permutation "s1b" "s2b1" "s1l" "s1u"        "s2l" "s1c"          "s1ul" "s2c"
permutation "s1b" "s2b1" "s1l" "s2l" "s1ul" "s1u" "s1c"                 "s2c"
permutation "s1b" "s2b1" "s1l" "s1u" "s1ul" "s2l" "s1c"                 "s2c"

permutation "s1b" "s2b1" "s1l" "s2l"        "s1u" "s1c" "s1hint" "s1ul" "s2c"
permutation "s1b" "s2b1" "s1l" "s1u"        "s2l" "s1c" "s1hint" "s1ul" "s2c"
permutation "s1b" "s2b1" "s1l" "s2l" "s1ul" "s1u" "s1c" "s1hint"        "s2c"
permutation "s1b" "s2b1" "s1l" "s1u" "s1ul" "s2l" "s1c" "s1hint"        "s2c"

permutation "s1b" "s2b2" "s1l" "s2l"        "s1u" "s1c"          "s1ul" "s2c"
permutation "s1b" "s2b2" "s1l" "s1u"        "s2l" "s1c"          "s1ul" "s2c"
permutation "s1b" "s2b2" "s1l" "s2l" "s1ul" "s1u" "s1c"                 "s2c"
permutation "s1b" "s2b2" "s1l" "s1u" "s1ul" "s2l" "s1c"                 "s2c"

permutation "s1b" "s2b2" "s1l" "s2l"        "s1u" "s1c" "s1hint" "s1ul" "s2c"
permutation "s1b" "s2b2" "s1l" "s1u"        "s2l" "s1c" "s1hint" "s1ul" "s2c"
permutation "s1b" "s2b2" "s1l" "s2l" "s1ul" "s1u" "s1c" "s1hint"        "s2c"
permutation "s1b" "s2b2" "s1l" "s1u" "s1ul" "s2l" "s1c" "s1hint"        "s2c"

permutation "s1b" "s2b3" "s1l" "s2l"        "s1u" "s1c"          "s1ul" "s2c"
permutation "s1b" "s2b3" "s1l" "s1u"        "s2l" "s1c"          "s1ul" "s2c"
permutation "s1b" "s2b3" "s1l" "s2l" "s1ul" "s1u" "s1c"                 "s2c"
permutation "s1b" "s2b3" "s1l" "s1u" "s1ul" "s2l" "s1c"                 "s2c"

permutation "s1b" "s2b3" "s1l" "s2l"        "s1u" "s1c" "s1hint" "s1ul" "s2c"
permutation "s1b" "s2b3" "s1l" "s1u"        "s2l" "s1c" "s1hint" "s1ul" "s2c"
permutation "s1b" "s2b3" "s1l" "s2l" "s1ul" "s1u" "s1c" "s1hint"        "s2c"
permutation "s1b" "s2b3" "s1l" "s1u" "s1ul" "s2l" "s1c" "s1hint"        "s2c"
