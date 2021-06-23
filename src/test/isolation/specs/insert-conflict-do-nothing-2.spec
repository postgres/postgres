# INSERT...ON CONFLICT DO NOTHING test with multiple rows
# in higher isolation levels

setup
{
  CREATE TABLE ints (key int, val text, PRIMARY KEY (key) INCLUDE (val));
}

teardown
{
  DROP TABLE ints;
}

session s1
step beginrr1 { BEGIN ISOLATION LEVEL REPEATABLE READ; }
step begins1 { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step donothing1 { INSERT INTO ints(key, val) VALUES(1, 'donothing1') ON CONFLICT DO NOTHING; }
step c1 { COMMIT; }
step show { SELECT * FROM ints; }

session s2
step beginrr2 { BEGIN ISOLATION LEVEL REPEATABLE READ; }
step begins2 { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step donothing2 { INSERT INTO ints(key, val) VALUES(1, 'donothing2'), (1, 'donothing3') ON CONFLICT DO NOTHING; }
step c2 { COMMIT; }

permutation beginrr1 beginrr2 donothing1 c1 donothing2 c2 show
permutation beginrr1 beginrr2 donothing2 c2 donothing1 c1 show
permutation beginrr1 beginrr2 donothing1 donothing2 c1 c2 show
permutation beginrr1 beginrr2 donothing2 donothing1 c2 c1 show
permutation begins1 begins2 donothing1 c1 donothing2 c2 show
permutation begins1 begins2 donothing2 c2 donothing1 c1 show
permutation begins1 begins2 donothing1 donothing2 c1 c2 show
permutation begins1 begins2 donothing2 donothing1 c2 c1 show
