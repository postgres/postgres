# Test write skew with a materialized view.
#
# This test uses two serializable transactions: one that refreshes a
# materialized view containing a summary of some order information, and
# one that looks at the materialized view while doing writes on its
# parent relation.
#
# Any overlap between the transactions should cause a serialization failure.

setup
{
  CREATE TABLE orders (date date, item text, num int);
  INSERT INTO orders VALUES ('2022-04-01', 'apple', 10), ('2022-04-01', 'banana', 20);

  CREATE MATERIALIZED VIEW order_summary AS
    SELECT date, item, sum(num) FROM orders GROUP BY date, item;
  CREATE UNIQUE INDEX ON order_summary(date, item);
  -- Create a diff between the summary table and the parent orders.
  INSERT INTO orders VALUES ('2022-04-02', 'apple', 20);
}

teardown
{
  DROP MATERIALIZED VIEW order_summary;
  DROP TABLE orders;
}

session s1
step s1_begin  { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1_refresh { REFRESH MATERIALIZED VIEW CONCURRENTLY order_summary; }
step s1_commit  { COMMIT; }

session s2
step s2_begin  { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2_read   { SELECT max(date) FROM order_summary; }
step s2_insert { INSERT INTO orders VALUES ('2022-04-02', 'orange', 15); }
step s2_update { UPDATE orders SET num = num + 1; }
step s2_commit { COMMIT; }

# refresh -> read -> write
permutation "s1_begin" "s2_begin" "s1_refresh" "s2_read" "s2_insert" "s1_commit" "s2_commit"
permutation "s1_begin" "s2_begin" "s1_refresh" "s2_read" "s2_update" "s1_commit" "s2_commit"
# read -> refresh -> write
permutation "s1_begin" "s2_begin" "s2_read" "s1_refresh" "s2_insert" "s1_commit" "s2_commit"
permutation "s1_begin" "s2_begin" "s2_read" "s1_refresh" "s2_update" "s1_commit" "s2_commit"
# read -> write -> refresh
permutation "s1_begin" "s2_begin" "s2_read" "s2_insert" "s1_refresh" "s1_commit" "s2_commit"
permutation "s1_begin" "s2_begin" "s2_read" "s2_update" "s1_refresh" "s1_commit" "s2_commit"
# refresh -> write -> read
permutation "s1_begin" "s2_begin" "s1_refresh" "s2_insert" "s2_read" "s1_commit" "s2_commit"
permutation "s1_begin" "s2_begin" "s1_refresh" "s2_update" "s2_read" "s1_commit" "s2_commit"
