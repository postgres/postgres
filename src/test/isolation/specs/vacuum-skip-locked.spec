# Test for SKIP_LOCKED option of VACUUM and ANALYZE commands.
#
# This also verifies that log messages are not emitted for skipped relations
# that were not specified in the VACUUM or ANALYZE command.

setup
{
	CREATE TABLE parted (a INT) PARTITION BY LIST (a);
	CREATE TABLE part1 PARTITION OF parted FOR VALUES IN (1);
	ALTER TABLE part1 SET (autovacuum_enabled = false);
	CREATE TABLE part2 PARTITION OF parted FOR VALUES IN (2);
	ALTER TABLE part2 SET (autovacuum_enabled = false);
}

teardown
{
	DROP TABLE IF EXISTS parted;
}

session "s1"
step "lock_share"
{
	BEGIN;
	LOCK part1 IN SHARE MODE;
}
step "lock_access_exclusive"
{
	BEGIN;
	LOCK part1 IN ACCESS EXCLUSIVE MODE;
}
step "commit"
{
	COMMIT;
}

session "s2"
step "vac_specified"		{ VACUUM (SKIP_LOCKED) part1, part2; }
step "vac_all_parts"		{ VACUUM (SKIP_LOCKED) parted; }
step "analyze_specified"	{ ANALYZE (SKIP_LOCKED) part1, part2; }
step "analyze_all_parts"	{ ANALYZE (SKIP_LOCKED) parted; }
step "vac_analyze_specified"	{ VACUUM (ANALYZE, SKIP_LOCKED) part1, part2; }
step "vac_analyze_all_parts"	{ VACUUM (ANALYZE, SKIP_LOCKED) parted; }
step "vac_full_specified"	{ VACUUM (SKIP_LOCKED, FULL) part1, part2; }
step "vac_full_all_parts"	{ VACUUM (SKIP_LOCKED, FULL) parted; }

permutation "lock_share" "vac_specified" "commit"
permutation "lock_share" "vac_all_parts" "commit"
permutation "lock_share" "analyze_specified" "commit"
permutation "lock_share" "analyze_all_parts" "commit"
permutation "lock_share" "vac_analyze_specified" "commit"
permutation "lock_share" "vac_analyze_all_parts" "commit"
permutation "lock_share" "vac_full_specified" "commit"
permutation "lock_share" "vac_full_all_parts" "commit"
permutation "lock_access_exclusive" "vac_specified" "commit"
permutation "lock_access_exclusive" "vac_all_parts" "commit"
permutation "lock_access_exclusive" "analyze_specified" "commit"
permutation "lock_access_exclusive" "analyze_all_parts" "commit"
permutation "lock_access_exclusive" "vac_analyze_specified" "commit"
permutation "lock_access_exclusive" "vac_analyze_all_parts" "commit"
permutation "lock_access_exclusive" "vac_full_specified" "commit"
permutation "lock_access_exclusive" "vac_full_all_parts" "commit"
