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
  create table gin_tbl(p int4[]);
  insert into gin_tbl select array[1] from generate_series(1, 8192) g;
  insert into gin_tbl select array[g] from generate_series(2, 800) g;
  create index ginidx on gin_tbl using gin(p) with (fastupdate = off);
  create table other_tbl(v int4);
}

teardown
{
  drop table gin_tbl;
  drop table other_tbl;
}

session s1
setup
{
  begin isolation level serializable;
  set enable_seqscan=off;
}

step ra1	{ select * from gin_tbl where p @> array[1] limit 1; }
step rb1  { select count(*) from gin_tbl where p @> array[2]; }
step rc1  { select count(*) from gin_tbl where p @> array[800]; }
step rd1  { select count(*) from gin_tbl where p @> array[2000]; }

step wo1	{ insert into other_tbl values (1); }

step c1  { commit; }

session s2
setup
{
  begin isolation level serializable;
  set enable_seqscan=off;
}

step ro2	{ select count(*) from other_tbl; }

step wa2  { insert into gin_tbl values (array[1]); }
step wb2  { insert into gin_tbl values (array[2]); }
step wc2  { insert into gin_tbl values (array[800]); }
step wd2  { insert into gin_tbl values (array[2000]); }

step c2  { commit; }

session s3
step fu { alter index ginidx set (fastupdate = on); }

# An index scan (from one transaction) and an index insert (from another
# transaction) try to access the same part of the index. So, there is a
# r-w conflict.

permutation ra1 ro2 wo1 c1 wa2 c2
permutation ro2 ra1 wo1 c1 wa2 c2
permutation ro2 ra1 wo1 wa2 c1 c2
permutation ra1 ro2 wa2 wo1 c1 c2

permutation rb1 ro2 wo1 c1 wb2 c2
permutation ro2 rb1 wo1 c1 wb2 c2
permutation ro2 rb1 wo1 wb2 c1 c2
permutation rb1 ro2 wb2 wo1 c1 c2

permutation rc1 ro2 wo1 c1 wc2 c2
permutation ro2 rc1 wo1 c1 wc2 c2
permutation ro2 rc1 wo1 wc2 c1 c2
permutation rc1 ro2 wc2 wo1 c1 c2

# An index scan (from one transaction) and an index insert (from another
# transaction) try to access different parts of the index.  So, there is no
# r-w conflict.

permutation ra1 ro2 wo1 c1 wb2 c2
permutation ro2 ra1 wo1 c1 wc2 c2
permutation ro2 rb1 wo1 wa2 c1 c2
permutation rc1 ro2 wa2 wo1 c1 c2

permutation rb1 ro2 wo1 c1 wa2 c2
permutation ro2 rb1 wo1 c1 wc2 c2
permutation ro2 ra1 wo1 wb2 c1 c2
permutation rc1 ro2 wb2 wo1 c1 c2

permutation rc1 ro2 wo1 c1 wa2 c2
permutation ro2 rc1 wo1 c1 wb2 c2
permutation ro2 ra1 wo1 wc2 c1 c2
permutation rb1 ro2 wc2 wo1 c1 c2

# With fastupdate = on all index is under predicate lock.  So we can't
# distinguish particular keys.

permutation fu ra1 ro2 wo1 c1 wa2 c2
permutation fu ra1 ro2 wo1 c1 wb2 c2

# Check fastupdate turned on concurrently.

permutation ra1 ro2 wo1 c1 fu wa2 c2

# Tests for conflicts with previously non-existing key

permutation rd1 ro2 wo1 c1 wd2 c2
permutation ro2 rd1 wo1 c1 wd2 c2
permutation ro2 rd1 wo1 wd2 c1 c2
permutation rd1 ro2 wd2 wo1 c1 c2
