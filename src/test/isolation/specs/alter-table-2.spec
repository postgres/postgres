# ALTER TABLE - Add foreign keys with concurrent reads
#
# ADD CONSTRAINT uses ShareRowExclusiveLock so we mix writes with it
# to see what works or waits.

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
step "s1a" { BEGIN; }
step "s1b" { ALTER TABLE b ADD CONSTRAINT bfk FOREIGN KEY (a_id) REFERENCES a (i) NOT VALID; }
step "s1c" { COMMIT; }

session "s2"
step "s2a" { BEGIN; }
step "s2b" { SELECT * FROM a WHERE i = 1 LIMIT 1 FOR UPDATE; }
step "s2c" { SELECT * FROM b WHERE a_id = 3 LIMIT 1 FOR UPDATE; }
step "s2d" { INSERT INTO b VALUES (0); }
step "s2e" { INSERT INTO a VALUES (4); }
step "s2f" { COMMIT; }
