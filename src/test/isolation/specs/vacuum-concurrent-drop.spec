# Test for log messages emitted by VACUUM and ANALYZE when a specified
# relation is concurrently dropped.
#
# This also verifies that log messages are not emitted for concurrently
# dropped relations that were not specified in the VACUUM or ANALYZE
# command.

setup
{
	CREATE TABLE parted (a INT) PARTITION BY LIST (a);
	CREATE TABLE part1 PARTITION OF parted FOR VALUES IN (1);
	CREATE TABLE part2 PARTITION OF parted FOR VALUES IN (2);
}

teardown
{
	DROP TABLE IF EXISTS parted;
}

session "s1"
step "lock"
{
	BEGIN;
	LOCK part1 IN SHARE MODE;
}
step "drop_and_commit"
{
	DROP TABLE part2;
	COMMIT;
}

session "s2"
step "vac_specified"		{ VACUUM part1, part2; }
step "vac_all_parts"		{ VACUUM parted; }
step "analyze_specified"	{ ANALYZE part1, part2; }
step "analyze_all_parts"	{ ANALYZE parted; }
step "vac_analyze_specified"	{ VACUUM ANALYZE part1, part2; }
step "vac_analyze_all_parts"	{ VACUUM ANALYZE parted; }

permutation "lock" "vac_specified" "drop_and_commit"
permutation "lock" "vac_all_parts" "drop_and_commit"
permutation "lock" "analyze_specified" "drop_and_commit"
permutation "lock" "analyze_all_parts" "drop_and_commit"
permutation "lock" "vac_analyze_specified" "drop_and_commit"
permutation "lock" "vac_analyze_all_parts" "drop_and_commit"
