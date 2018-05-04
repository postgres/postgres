#
# Test that predicate locking on a GIN index works correctly, even if
# fastupdate is turned on concurrently.
#
# 0. fastupdate is off
# 1. Session 's1' acquires predicate lock on page X
# 2. fastupdate is turned on
# 3. Session 's2' inserts a new tuple to the pending list
#
# This test tests that if the lock acquired in step 1 would conflict with
# the scan in step 1, we detect that conflict correctly, even if fastupdate
# was turned on in-between.
#
setup
{
  create table gin_tbl(p int4[]);
  insert into gin_tbl select array[g, g*2,g*3] from generate_series(1, 10000) g;
  insert into gin_tbl select array[4,5,6] from generate_series(10001, 20000) g;
  create index ginidx on gin_tbl using gin(p) with (fastupdate = off);

  create table other_tbl (id int4);
}

teardown
{
  drop table gin_tbl;
  drop table other_tbl;
}

session "s1"
setup { BEGIN ISOLATION LEVEL SERIALIZABLE; SET enable_seqscan=off; }
step "r1" { SELECT count(*) FROM gin_tbl WHERE p @> array[1000]; }
step "w1" { INSERT INTO other_tbl VALUES (42); }
step "c1" { COMMIT; }

session "s2"
setup { BEGIN ISOLATION LEVEL SERIALIZABLE; SET enable_seqscan=off; }
step "r2" { SELECT * FROM other_tbl; }
step "w2" { INSERT INTO gin_tbl SELECT array[1000,19001]; }
step "c2" { COMMIT; }

session "s3"
step "fastupdate_on" { ALTER INDEX ginidx SET (fastupdate = on); }

# This correctly throws serialization failure.
permutation "r1" "r2" "w1" "c1" "w2" "c2"

# But if fastupdate is turned on in the middle, we miss it.
permutation "r1" "r2" "w1" "c1" "fastupdate_on" "w2" "c2"
