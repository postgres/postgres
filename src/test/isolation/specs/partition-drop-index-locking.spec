# Verify that DROP INDEX properly locks all downward sub-partitions
# and partitions before locking the indexes.

setup
{
  CREATE TABLE part_drop_index_locking (id int) PARTITION BY RANGE(id);
  CREATE TABLE part_drop_index_locking_subpart PARTITION OF part_drop_index_locking FOR VALUES FROM (1) TO (100) PARTITION BY RANGE(id);
  CREATE TABLE part_drop_index_locking_subpart_child PARTITION OF part_drop_index_locking_subpart FOR VALUES FROM (1) TO (100);
  CREATE INDEX part_drop_index_locking_idx ON part_drop_index_locking(id);
  CREATE INDEX part_drop_index_locking_subpart_idx ON part_drop_index_locking_subpart(id);
}

teardown
{
  DROP TABLE part_drop_index_locking;
}

# SELECT will take AccessShare lock first on the table and then on its index.
# We can simulate the case where DROP INDEX starts between those steps
# by manually taking the table lock beforehand.
session s1
step s1begin    { BEGIN; }
step s1lock     { LOCK TABLE part_drop_index_locking_subpart_child IN ACCESS SHARE MODE; }
step s1select   { SELECT * FROM part_drop_index_locking_subpart_child; }
step s1commit   { COMMIT; }

session s2
step s2begin    { BEGIN; }
step s2drop     { DROP INDEX part_drop_index_locking_idx; }
step s2dropsub  { DROP INDEX part_drop_index_locking_subpart_idx; }
step s2commit   { COMMIT; }

session s3
step s3getlocks {
        SELECT s.query, c.relname, l.mode, l.granted
        FROM pg_locks l
                JOIN pg_class c ON l.relation = c.oid
                JOIN pg_stat_activity s ON l.pid = s.pid
        WHERE c.relname LIKE 'part_drop_index_locking%'
        ORDER BY s.query, c.relname, l.mode, l.granted;
}

# Run DROP INDEX on top partitioned table
permutation s1begin s1lock s2begin s2drop(s1commit) s1select s3getlocks s1commit s3getlocks s2commit

# Run DROP INDEX on top sub-partition table
permutation s1begin s1lock s2begin s2dropsub(s1commit) s1select s3getlocks s1commit s3getlocks s2commit
