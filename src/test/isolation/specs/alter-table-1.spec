# ALTER TABLE - Add and Validate constraint with concurrent writes
#
# VALIDATE allows a minimum of ShareUpdateExclusiveLock
# so we mix reads with it to see what works or waits

setup
{
 CREATE TABLE a (i int PRIMARY KEY);
 CREATE TABLE b (a_id int);
 INSERT INTO a VALUES (0), (1), (2), (3);
 INSERT INTO b SELECT generate_series(1,1000) % 4;
}

teardown
{
 DROP TABLE a, b;
}

session "s1"
step "s1"	{ BEGIN; }
step "at1"	{ ALTER TABLE b ADD CONSTRAINT bfk FOREIGN KEY (a_id) REFERENCES a (i) NOT VALID; }
step "sc1"	{ COMMIT; }
step "s2"	{ BEGIN; }
step "at2"	{ ALTER TABLE b VALIDATE CONSTRAINT bfk; }
step "sc2"	{ COMMIT; }

session "s2"
setup		{ BEGIN; }
step "rx1"	{ SELECT * FROM b WHERE a_id = 1 LIMIT 1; }
step "wx"	{ INSERT INTO b VALUES (0); }
step "rx1"	{ SELECT * FROM b WHERE a_id = 3 LIMIT 3; }
step "c2"	{ COMMIT; }
