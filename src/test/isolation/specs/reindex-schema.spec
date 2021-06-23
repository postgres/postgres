# REINDEX with schemas
#
# Check that concurrent drop of relations while doing a REINDEX
# SCHEMA allows the command to work.

setup
{
    CREATE SCHEMA reindex_schema;
    CREATE TABLE reindex_schema.tab_locked (a int PRIMARY KEY);
    CREATE TABLE reindex_schema.tab_dropped (a int PRIMARY KEY);
}

teardown
{
    DROP SCHEMA reindex_schema CASCADE;
}

session s1
step begin1        { BEGIN; }
step lock1         { LOCK reindex_schema.tab_locked IN SHARE UPDATE EXCLUSIVE MODE; }
step end1          { COMMIT; }

session s2
step reindex2      { REINDEX SCHEMA reindex_schema; }
step reindex_conc2 { REINDEX SCHEMA CONCURRENTLY reindex_schema; }

session s3
step drop3         { DROP TABLE reindex_schema.tab_dropped; }

# The table can be dropped while reindex is waiting.
permutation begin1 lock1 reindex2 drop3 end1
permutation begin1 lock1 reindex_conc2 drop3 end1
