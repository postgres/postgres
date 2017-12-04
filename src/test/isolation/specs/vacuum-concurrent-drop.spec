# Test for log messages emitted by VACUUM and ANALYZE when a specified
# relation is concurrently dropped.
#
# This also verifies that log messages are not emitted for concurrently
# dropped relations that were not specified in the VACUUM or ANALYZE
# command.

setup
{
	CREATE TABLE test1 (a INT);
	CREATE TABLE test2 (a INT);
}

teardown
{
	DROP TABLE IF EXISTS test1;
	DROP TABLE IF EXISTS test2;
}

session "s1"
step "lock"
{
	BEGIN;
	LOCK test1 IN SHARE MODE;
}
step "drop_and_commit"
{
	DROP TABLE test2;
	COMMIT;
}

session "s2"
step "vac_specified"		{ VACUUM test1, test2; }
step "vac_all"			{ VACUUM; }
step "analyze_specified"	{ ANALYZE test1, test2; }
step "analyze_all"		{ ANALYZE; }
step "vac_analyze_specified"	{ VACUUM ANALYZE test1, test2; }
step "vac_analyze_all"		{ VACUUM ANALYZE; }

permutation "lock" "vac_specified" "drop_and_commit"
permutation "lock" "vac_all" "drop_and_commit"
permutation "lock" "analyze_specified" "drop_and_commit"
permutation "lock" "analyze_all" "drop_and_commit"
permutation "lock" "vac_analyze_specified" "drop_and_commit"
permutation "lock" "vac_analyze_all" "drop_and_commit"
