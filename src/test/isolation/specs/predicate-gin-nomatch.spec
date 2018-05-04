#
# Check that GIN index grabs an appropriate lock, even if there is no match.
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
# Scan with no match.
step "r1" { SELECT count(*) FROM gin_tbl WHERE p @> array[-1]; }
step "w1" { INSERT INTO other_tbl VALUES (42); }
step "c1" { COMMIT; }

session "s2"
setup { BEGIN ISOLATION LEVEL SERIALIZABLE; SET enable_seqscan=off; }
step "r2" { SELECT * FROM other_tbl; }
# Insert row that would've matched in step "r1"
step "w2" { INSERT INTO gin_tbl SELECT array[-1]; }
step "c2" { COMMIT; }

# This should throw serialization failure.
permutation "r1" "r2" "w1" "c1" "w2" "c2"
