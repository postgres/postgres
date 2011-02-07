# Two IDs test
#
# Small, simple test showing read-only anomalies.
#
# There are only four permuations which must cause a serialization failure.
# Required failure cases are where s2 overlaps both s1 and s3, but s1
# commits before s3 executes its first SELECT.
#
# If s3 were declared READ ONLY there would be no false positives.
# With s3 defaulting to READ WRITE, we currently expect 12 false
# positives.  Further work dealing with de facto READ ONLY transactions
# may be able to reduce or eliminate those false positives.

setup
{
 create table D1 (id int not null);
 create table D2 (id int not null);
 insert into D1 values (1);
 insert into D2 values (1);
}

teardown
{
 DROP TABLE D1, D2;
}

session "s1"
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step "wx1"	{ update D1 set id = id + 1; }
step "c1"	{ COMMIT; }

session "s2"
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step "rxwy2"	{ update D2 set id = (select id+1 from D1); }
step "c2"	{ COMMIT; }

session "s3"
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step "ry3"	{ select id from D2; }
step "c3"	{ COMMIT; }
