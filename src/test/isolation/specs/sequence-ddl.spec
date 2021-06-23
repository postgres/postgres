# Test sequence usage and concurrent sequence DDL

setup
{
    CREATE SEQUENCE seq1;
}

teardown
{
    DROP SEQUENCE seq1;
}

session s1
setup           { BEGIN; }
step s1alter    { ALTER SEQUENCE seq1 MAXVALUE 10; }
step s1alter2   { ALTER SEQUENCE seq1 MAXVALUE 20; }
step s1restart  { ALTER SEQUENCE seq1 RESTART WITH 5; }
step s1commit   { COMMIT; }

session s2
step s2begin    { BEGIN; }
step s2nv       { SELECT nextval('seq1') FROM generate_series(1, 15); }
step s2commit   { COMMIT; }

permutation s1alter s1commit s2nv

# Prior to PG10, the s2nv step would see the uncommitted s1alter
# change, but now it waits.
permutation s1alter s2nv s1commit

# Prior to PG10, the s2nv step would see the uncommitted s1restart
# change, but now it waits.
permutation s1restart s2nv s1commit

# In contrast to ALTER setval() is non-transactional, so it doesn't
# have to wait.
permutation s1restart s2nv s1commit

# nextval doesn't release lock until transaction end, so s1alter2 has
# to wait for s2commit.
permutation s2begin s2nv s1alter2 s2commit s1commit
