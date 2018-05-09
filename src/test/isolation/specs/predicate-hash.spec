# Test for page level predicate locking in hash index
#
# Test to verify serialization failures and to check reduced false positives
#
# To verify serialization failures, queries and permutations are written in such
# a way that an index scan  (from one transaction) and an index insert (from
# another transaction) will try to access the same bucket of the index
# whereas to check reduced false positives, they will try to access different
# buckets of the index.

setup
{
 create table hash_tbl(id int4, p integer);
 create index hash_idx on hash_tbl using hash(p);
 insert into hash_tbl (id, p)
 select g, 10 from generate_series(1, 10) g;
 insert into hash_tbl (id, p)
 select g, 20 from generate_series(11, 20) g;
 insert into hash_tbl (id, p)
 select g, 30 from generate_series(21, 30) g;
 insert into hash_tbl (id, p)
 select g, 40 from generate_series(31, 40) g;
}

teardown
{
 drop table hash_tbl;
}

session "s1"
setup
{
 begin isolation level serializable;
 set enable_seqscan=off;
 set enable_bitmapscan=off;
 set enable_indexonlyscan=on;
}
step "rxy1"	{ select sum(p) from hash_tbl where p=20; }
step "wx1"	{ insert into hash_tbl (id, p)
			  select g, 30 from generate_series(41, 50) g; }
step "rxy3"	{ select sum(p) from hash_tbl where p=20; }
step "wx3"	{ insert into hash_tbl (id, p)
			  select g, 50 from generate_series(41, 50) g; }
step "c1"	{ commit; }


session "s2"
setup
{
 begin isolation level serializable;
 set enable_seqscan=off;
 set enable_bitmapscan=off;
 set enable_indexonlyscan=on;
}
step "rxy2"	{ select sum(p) from hash_tbl where p=30; }
step "wy2"	{ insert into hash_tbl (id, p)
			  select g, 20 from generate_series(51, 60) g; }
step "rxy4"	{ select sum(p) from hash_tbl where p=30; }
step "wy4"	{ insert into hash_tbl (id, p)
			  select g, 60 from generate_series(51, 60) g; }
step "c2"	{ commit; }


# An index scan (from one transaction) and an index insert (from another
# transaction) try to access the same bucket of the index but one transaction
# commits before other transaction begins so no r-w conflict.

permutation "rxy1" "wx1" "c1" "rxy2" "wy2" "c2"
permutation "rxy2" "wy2" "c2" "rxy1" "wx1" "c1"

# An index scan (from one transaction) and an index insert (from another
# transaction) try to access different buckets of the index and also one
# transaction commits before other transaction begins, so no r-w conflict.

permutation "rxy3" "wx3" "c1" "rxy4" "wy4" "c2"
permutation "rxy4" "wy4" "c2" "rxy3" "wx3" "c1"


# An index scan (from one transaction) and an index insert (from another
# transaction) try to access the same bucket of the index and one transaction
# begins before other transaction commits so there is a r-w conflict.

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

# An index scan (from one transaction) and an index insert (from another
# transaction) try to access different buckets of the index so no r-w conflict.

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
