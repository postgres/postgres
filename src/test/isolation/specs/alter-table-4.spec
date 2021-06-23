# ALTER TABLE - Add and remove inheritance with concurrent reads

setup
{
 CREATE TABLE p (a integer);
 INSERT INTO p VALUES(1);
 CREATE TABLE c1 () INHERITS (p);
 INSERT INTO c1 VALUES(10);
 CREATE TABLE c2 (a integer);
 INSERT INTO c2 VALUES(100);
}

teardown
{
 DROP TABLE IF EXISTS c1, c2, p;
}

session s1
step s1b	{ BEGIN; }
step s1delc1	{ ALTER TABLE c1 NO INHERIT p; }
step s1modc1a	{ ALTER TABLE c1 ALTER COLUMN a TYPE float; }
step s1addc2	{ ALTER TABLE c2 INHERIT p; }
step s1dropc1	{ DROP TABLE c1; }
step s1c	{ COMMIT; }

session s2
step s2sel	{ SELECT SUM(a) FROM p; }

# NO INHERIT will not be visible to concurrent select,
# since we identify children before locking them
permutation s1b s1delc1 s2sel s1c s2sel
# adding inheritance likewise is not seen if s1 commits after s2 locks p
permutation s1b s1delc1 s1addc2 s2sel s1c s2sel
# but we do cope with DROP on a child table
permutation s1b s1dropc1 s2sel s1c s2sel
# this case currently results in an error; doesn't seem worth preventing
permutation s1b s1delc1 s1modc1a s2sel s1c s2sel
