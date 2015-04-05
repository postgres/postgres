# ALTER TABLE - Enable and disable triggers with concurrent reads
#
# ENABLE/DISABLE TRIGGER uses ShareRowExclusiveLock so we mix writes with
# it to see what works or waits.

setup
{
 CREATE TABLE a (i int PRIMARY KEY);
 INSERT INTO a VALUES (0), (1), (2), (3);
 CREATE FUNCTION f() RETURNS TRIGGER LANGUAGE plpgsql AS 'BEGIN RETURN NULL; END;';
 CREATE TRIGGER t AFTER UPDATE ON a EXECUTE PROCEDURE f();
}

teardown
{
 DROP TABLE a;
 DROP FUNCTION f();
}

session "s1"
step "s1a" { BEGIN; }
step "s1b" { ALTER TABLE a DISABLE TRIGGER t; }
step "s1c" { ALTER TABLE a ENABLE TRIGGER t; }
step "s1d" { COMMIT; }

session "s2"
step "s2a" { BEGIN; }
step "s2b" { SELECT * FROM a WHERE i = 1 LIMIT 1 FOR UPDATE; }
step "s2c" { INSERT INTO a VALUES (0); }
step "s2d" { COMMIT; }
