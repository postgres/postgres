# Test for page level predicate locking in gist
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
  create table gist_point_tbl(id int4, p point);
  create index gist_pointidx on gist_point_tbl using gist(p);
  insert into gist_point_tbl (id, p)
  select g, point(g*10, g*10) from generate_series(1, 1000) g;
}

teardown
{
  drop table gist_point_tbl;
}

session s1
setup
{
  begin isolation level serializable;
  set enable_seqscan=off;
  set enable_bitmapscan=off;
  set enable_indexonlyscan=on;
}

step rxy1	{ select sum(p[0]) from gist_point_tbl where p << point(2500, 2500); }
step wx1	{ insert into gist_point_tbl (id, p)
			  select g, point(g*500, g*500) from generate_series(15, 20) g; }
step rxy3	{ select sum(p[0]) from gist_point_tbl where p >> point(6000,6000); }
step wx3	{ insert into gist_point_tbl (id, p)
			  select g, point(g*500, g*500) from generate_series(12, 18) g; }
step c1		{ commit; }


session s2
setup
{
  begin isolation level serializable;
  set enable_seqscan=off;
  set enable_bitmapscan=off;
  set enable_indexonlyscan=on;
}

step rxy2	{ select sum(p[0]) from gist_point_tbl where p >> point(7500,7500); }
step wy2	{ insert into gist_point_tbl (id, p)
			  select g, point(g*500, g*500) from generate_series(1, 5) g; }
step rxy4	{ select sum(p[0]) from gist_point_tbl where p << point(1000,1000); }
step wy4	{ insert into gist_point_tbl (id, p)
			  select g, point(g*50, g*50) from generate_series(1, 20) g; }
step c2		{ commit; }

# An index scan (from one transaction) and an index insert (from another
# transaction) try to access the same part of the index but one transaction
# commits before other transaction begins so no r-w conflict.

permutation rxy1 wx1 c1 rxy2 wy2 c2
permutation rxy2 wy2 c2 rxy1 wx1 c1

# An index scan (from one transaction) and an index insert (from another
# transaction) try to access different parts of the index and also one
# transaction commits before other transaction begins, so no r-w conflict.

permutation rxy3 wx3 c1 rxy4 wy4 c2
permutation rxy4 wy4 c2 rxy3 wx3 c1


# An index scan (from one transaction) and an index insert (from another
# transaction) try to access the same part of the index and one transaction
# begins before other transaction commits so there is a r-w conflict.

permutation rxy1 wx1 rxy2 c1 wy2 c2
permutation rxy1 wx1 rxy2 wy2 c1 c2
permutation rxy1 wx1 rxy2 wy2 c2 c1
permutation rxy1 rxy2 wx1 c1 wy2 c2
permutation rxy1 rxy2 wx1 wy2 c1 c2
permutation rxy1 rxy2 wx1 wy2 c2 c1
permutation rxy1 rxy2 wy2 wx1 c1 c2
permutation rxy1 rxy2 wy2 wx1 c2 c1
permutation rxy1 rxy2 wy2 c2 wx1 c1
permutation rxy2 rxy1 wx1 c1 wy2 c2
permutation rxy2 rxy1 wx1 wy2 c1 c2
permutation rxy2 rxy1 wx1 wy2 c2 c1
permutation rxy2 rxy1 wy2 wx1 c1 c2
permutation rxy2 rxy1 wy2 wx1 c2 c1
permutation rxy2 rxy1 wy2 c2 wx1 c1
permutation rxy2 wy2 rxy1 wx1 c1 c2
permutation rxy2 wy2 rxy1 wx1 c2 c1
permutation rxy2 wy2 rxy1 c2 wx1 c1

# An index scan (from one transaction) and an index insert (from another
# transaction) try to access different parts of the index so no r-w conflict.

permutation rxy3 wx3 rxy4 c1 wy4 c2
permutation rxy3 wx3 rxy4 wy4 c1 c2
permutation rxy3 wx3 rxy4 wy4 c2 c1
permutation rxy3 rxy4 wx3 c1 wy4 c2
permutation rxy3 rxy4 wx3 wy4 c1 c2
permutation rxy3 rxy4 wx3 wy4 c2 c1
permutation rxy3 rxy4 wy4 wx3 c1 c2
permutation rxy3 rxy4 wy4 wx3 c2 c1
permutation rxy3 rxy4 wy4 c2 wx3 c1
permutation rxy4 rxy3 wx3 c1 wy4 c2
permutation rxy4 rxy3 wx3 wy4 c1 c2
permutation rxy4 rxy3 wx3 wy4 c2 c1
permutation rxy4 rxy3 wy4 wx3 c1 c2
permutation rxy4 rxy3 wy4 wx3 c2 c1
permutation rxy4 rxy3 wy4 c2 wx3 c1
permutation rxy4 wy4 rxy3 wx3 c1 c2
permutation rxy4 wy4 rxy3 wx3 c2 c1
permutation rxy4 wy4 rxy3 c2 wx3 c1
