# Test search_path invalidation.

setup
{
    CREATE USER regress_sp_user1;
    CREATE SCHEMA regress_sp_user1 AUTHORIZATION regress_sp_user1;
    CREATE SCHEMA regress_sp_public;
    GRANT ALL PRIVILEGES ON SCHEMA regress_sp_public TO regress_sp_user1;
}

teardown
{
    DROP SCHEMA regress_sp_public CASCADE;
    DROP SCHEMA regress_sp_user1 CASCADE;
    DROP USER regress_sp_user1;
}

session s1
setup
{
    SET search_path = "$user", regress_sp_public;
    SET SESSION AUTHORIZATION regress_sp_user1;
    CREATE TABLE regress_sp_user1.x(t) AS SELECT 'data in regress_sp_user1.x';
    CREATE TABLE regress_sp_public.x(t) AS SELECT 'data in regress_sp_public.x';
}
step s1a
{
    SELECT CURRENT_USER;
    SHOW search_path;
    SELECT t FROM x;
}

session s2
step s2a
{
    ALTER ROLE regress_sp_user1 RENAME TO regress_sp_user2;
}
step s2b
{
    ALTER ROLE regress_sp_user2 RENAME TO regress_sp_user1;
}

session s3
step s3a
{
    ALTER SCHEMA regress_sp_user1 RENAME TO regress_sp_user2;
}
step s3b
{
    ALTER SCHEMA regress_sp_user2 RENAME TO regress_sp_user1;
}

# s1's search_path is invalidated by role name change in s2a, and
# falls back to regress_sp_public.x
permutation s1a s2a s1a s2b

# s1's search_path is invalidated by schema name change in s2b, and
# falls back to regress_sp_public.x
permutation s1a s3a s1a s3b
