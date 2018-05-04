# Test for page level predicate locking in gin index
#
# Test to verify serialization failures and to check reduced false positives
#
# To verify serialization failures, queries and permutations are written in such
# a way that an index scan  (from one transaction) and an index insert (from
# another transaction) will try to access the same part (sub-tree) of the index
# whereas to check reduced false positives, they will try to access different
# parts (sub-tree) of the index.


setup
{
  create table gin_tbl(id int4, p int4[]);
  insert into gin_tbl select g, array[g, g*2,g*3] from generate_series(1, 10000) g;
  insert into gin_tbl select g, array[4,5,6] from generate_series(10001, 20000) g;
  create index ginidx on gin_tbl using gin(p) with (fastupdate = off);
}

teardown
{
  drop table gin_tbl;
}

session "s1"
setup
{
  begin isolation level serializable;
  set enable_seqscan=off;
}

# enable pending list for a small subset of tests
step "fu1"	{ alter index ginidx set (fastupdate = on);
			  commit;
			  begin isolation level serializable;
			  set enable_seqscan=off; }

step "rxy1"	{ select count(*) from gin_tbl where p @> array[4,5]; }
step "wx1"	{ insert into gin_tbl select g, array[5,6] from generate_series
              (20001, 20050) g; }
step "rxy3"	{ select count(*) from gin_tbl where p @> array[1,2] or
              p @> array[100,200] or p @> array[500,1000] or p @> array[1000,2000]; }
step "wx3"	{ insert into gin_tbl select g, array[g,g*2] from generate_series
              (1, 50) g; }
step "c1"  { commit; }

session "s2"
setup
{
  begin isolation level serializable;
  set enable_seqscan=off;
}

step "rxy2"	{ select count(*) from gin_tbl where p @> array[5,6]; }
step "rxy2fu"	{ select count(*) from gin_tbl where p @> array[10000,10005]; }
step "wy2"	{ insert into gin_tbl select g, array[4,5] from
              generate_series(20051, 20100) g; }
step "wy2fu"	{ insert into gin_tbl select g, array[10000,10005] from
              generate_series(20051, 20100) g; }
step "rxy4"	{ select count(*) from gin_tbl where p @> array[4000,8000] or
              p @> array[5000,10000] or p @> array[6000,12000] or
              p @> array[8000,16000]; }
step "wy4"	{ insert into gin_tbl select g, array[g,g*2] from generate_series
              (10000, 10050) g; }
step "c2"	{ commit; }


# An index scan (from one transaction) and an index insert (from another transaction)
# try to access the same part of the index but one transaction commits before other
# transaction begins so no r-w conflict.

permutation "rxy1" "wx1" "c1" "rxy2" "wy2" "c2"
permutation "rxy2" "wy2" "c2" "rxy1" "wx1" "c1"

# An index scan (from one transaction) and an index insert (from another transaction)
# try to access different parts of the index and also one transaction commits before
# other transaction begins, so no r-w conflict.

permutation "rxy3" "wx3" "c1" "rxy4" "wy4" "c2"
permutation "rxy4" "wy4" "c2" "rxy3" "wx3" "c1"


# An index scan (from one transaction) and an index insert (from another transaction)
# try to access the same part of the index and one transaction begins before other
# transaction commits so there is a r-w conflict.

permutation "rxy1" "wx1" "rxy2" "c1" "wy2" "c2"
permutation "rxy1" "wx1" "rxy2" "wy2" "c1" "c2"
permutation "rxy1" "wx1" "rxy2" "wy2" "c2" "c1"
permutation "rxy1" "rxy2" "wx1" "c1" "wy2" "c2"
permutation "rxy1" "rxy2" "wx1" "wy2" "c1" "c2"
permutation "rxy1" "rxy2" "wx1" "wy2" "c2" "c1"
permutation "rxy1" "rxy2" "wy2" "wx1" "c1" "c2"
permutation "rxy1" "rxy2" "wy2" "wx1" "c2" "c1"
permutation "rxy1" "rxy2" "wy2" "c2" "wx1" "c1"
permutation "rxy2" "rxy1" "wx1" "c1" "wy2" "c2"
permutation "rxy2" "rxy1" "wx1" "wy2" "c1" "c2"
permutation "rxy2" "rxy1" "wx1" "wy2" "c2" "c1"
permutation "rxy2" "rxy1" "wy2" "wx1" "c1" "c2"
permutation "rxy2" "rxy1" "wy2" "wx1" "c2" "c1"
permutation "rxy2" "rxy1" "wy2" "c2" "wx1" "c1"
permutation "rxy2" "wy2" "rxy1" "wx1" "c1" "c2"
permutation "rxy2" "wy2" "rxy1" "wx1" "c2" "c1"
permutation "rxy2" "wy2" "rxy1" "c2" "wx1" "c1"

# An index scan (from one transaction) and an index insert (from another transaction)
# try to access different parts of the index so no r-w conflict.

permutation "rxy3" "wx3" "rxy4" "c1" "wy4" "c2"
permutation "rxy3" "wx3" "rxy4" "wy4" "c1" "c2"
permutation "rxy3" "wx3" "rxy4" "wy4" "c2" "c1"
permutation "rxy3" "rxy4" "wx3" "c1" "wy4" "c2"
permutation "rxy3" "rxy4" "wx3" "wy4" "c1" "c2"
permutation "rxy3" "rxy4" "wx3" "wy4" "c2" "c1"
permutation "rxy3" "rxy4" "wy4" "wx3" "c1" "c2"
permutation "rxy3" "rxy4" "wy4" "wx3" "c2" "c1"
permutation "rxy3" "rxy4" "wy4" "c2" "wx3" "c1"
permutation "rxy4" "rxy3" "wx3" "c1" "wy4" "c2"
permutation "rxy4" "rxy3" "wx3" "wy4" "c1" "c2"
permutation "rxy4" "rxy3" "wx3" "wy4" "c2" "c1"
permutation "rxy4" "rxy3" "wy4" "wx3" "c1" "c2"
permutation "rxy4" "rxy3" "wy4" "wx3" "c2" "c1"
permutation "rxy4" "rxy3" "wy4" "c2" "wx3" "c1"
permutation "rxy4" "wy4" "rxy3" "wx3" "c1" "c2"
permutation "rxy4" "wy4" "rxy3" "wx3" "c2" "c1"
permutation "rxy4" "wy4" "rxy3" "c2" "wx3" "c1"

# Test fastupdate = on. First test should pass because fastupdate is off and
# sessions touches different parts of index, second should fail because
# with fastupdate on, then whole index should be under predicate lock.

permutation       "rxy1" "rxy2fu" "wx1" "c1" "wy2fu" "c2"
permutation "fu1" "rxy1" "rxy2fu" "wx1" "c1" "wy2fu" "c2"

