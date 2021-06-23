# Partial Index test
#
# Make sure that an update which moves a row out of a partial index
# is handled correctly.  In early versions, an attempt at optimization
# broke this behavior, allowing anomalies.
#
# Any overlap between the transactions must cause a serialization failure.

setup
{
 create table test_t (id integer, val1 text, val2 integer);
 create index test_idx on test_t(id) where val2 = 1;
 insert into test_t (select generate_series(0, 10000), 'a', 2);
 insert into test_t (select generate_series(0, 10), 'a', 1);
}

teardown
{
 DROP TABLE test_t;
}

session s1
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step rxy1	{ select * from test_t where val2 = 1; }
step wx1	{ update test_t set val2 = 2 where val2 = 1 and id = 10; }
step c1		{ COMMIT; }

session s2
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step wy2	{ update test_t set val2 = 2 where val2 = 1 and id = 9; }
step rxy2	{ select * from test_t where val2 = 1; }
step c2		{ COMMIT; }
