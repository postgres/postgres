# CREATE TRIGGER - Add trigger with concurrent reads
#
# CREATE TRIGGER uses ShareRowExclusiveLock so we mix writes with it
# to see what works or waits.

setup
{
 CREATE TABLE a (i int);
 CREATE FUNCTION f() RETURNS TRIGGER LANGUAGE plpgsql AS 'BEGIN RETURN NULL; END;';
 INSERT INTO a VALUES (0), (1), (2), (3);
}

teardown
{
 DROP TABLE a;
 DROP FUNCTION f();
}

session "s1"
step "s1a" { BEGIN; }
step "s1b" { CREATE TRIGGER t AFTER UPDATE ON a EXECUTE PROCEDURE f(); }
step "s1c" { COMMIT; }

session "s2"
step "s2a" { BEGIN; }
step "s2b" { SELECT * FROM a WHERE i = 1 FOR UPDATE; }
step "s2c" { UPDATE a SET i = 4 WHERE i = 3; }
step "s2d" { COMMIT; }
