# INSERT ... ON CONFLICT arbiter probe vs. a concurrent writer at SERIALIZABLE
#
# The arbiter probe reads the conflicting row to decide the statement's
# outcome, so the read must be visible to SSI atomically: any SIREAD lock
# taken after the probe leaves a window in which a concurrent writer of
# the row checks for conflicts without seeing it.  The injection point
# holds the statement right after the probe picked the conflicting tuple,
# while s2 updates the row and commits.  ON CONFLICT then decides based
# on a row version that s2 already replaced, completing the write-skew
# cycle: s1 reads a and writes b, s2 reads b and writes a.  s1 must fail.
# DO SELECT without a lock and with FOR KEY SHARE (which does not conflict
# with the non-key update) rely on SSI alone to notice this.

setup
{
	CREATE EXTENSION injection_points;
	CREATE TABLE probe_a (key int PRIMARY KEY, val int);
	CREATE TABLE probe_b (key int PRIMARY KEY, val int);
	INSERT INTO probe_a VALUES (1, 0);
	-- attached globally, the point has to fire in session s1
	SELECT injection_points_attach('check-exclusion-or-unique-constraint-conflict', 'wait');
}

teardown
{
	DROP TABLE probe_a, probe_b;
	DROP EXTENSION injection_points;
}

session s1
setup
{
	BEGIN ISOLATION LEVEL SERIALIZABLE;
}
step ioc_nothing1 { INSERT INTO probe_a VALUES (1, 99) ON CONFLICT (key) DO NOTHING; }
step ioc_select1 { INSERT INTO probe_a VALUES (1, 99) ON CONFLICT (key) DO SELECT RETURNING val; }
step ioc_select1_keyshare { INSERT INTO probe_a VALUES (1, 99) ON CONFLICT (key) DO SELECT FOR KEY SHARE RETURNING val; }
step insert1 { INSERT INTO probe_b VALUES (1, 10); }
step c1 { COMMIT; }

session s2
setup
{
	BEGIN ISOLATION LEVEL SERIALIZABLE;
}
step count2 { SELECT count(*) FROM probe_b; }
step update2 { UPDATE probe_a SET val = 1 WHERE key = 1; }
step c2 { COMMIT; }
step wake2
{
	SELECT injection_points_detach('check-exclusion-or-unique-constraint-conflict');
	SELECT injection_points_wakeup('check-exclusion-or-unique-constraint-conflict');
}

permutation ioc_nothing1 count2 update2 c2 wake2 insert1 c1
permutation ioc_select1 count2 update2 c2 wake2 insert1 c1
permutation ioc_select1_keyshare count2 update2 c2 wake2 insert1 c1
