# Tests for contrib/tcn

# These tests use only self-notifies within a single session,
# which are convenient because they minimize timing concerns.
# Whether the NOTIFY mechanism works across sessions is not
# really tcn's problem.

setup
{
  CREATE TABLE mytable (key int PRIMARY KEY, value text);
  CREATE TRIGGER tcntrig AFTER INSERT OR UPDATE OR DELETE ON mytable
    FOR EACH ROW EXECUTE FUNCTION triggered_change_notification(mychannel);
}

teardown
{
  DROP TABLE mytable;
}

session s1
step listen	{ LISTEN mychannel; }
step insert	{ INSERT INTO mytable VALUES(1, 'one'); }
step insert2	{ INSERT INTO mytable VALUES(2, 'two'); }
step update	{ UPDATE mytable SET value = 'foo' WHERE key = 2; }
step delete	{ DELETE FROM mytable; }


permutation listen insert insert2 update delete
