
# Copyright (c) 2016-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');

$node->init;
$node->append_conf(
	'postgresql.conf',
	qq{shared_preload_libraries = 'pg_plan_advice, pg_stash_advice'
pg_stash_advice.persist = true
pg_stash_advice.persist_interval = 0});
$node->start;

$node->safe_psql("postgres",
		"CREATE EXTENSION pg_stash_advice;\n");

# Create two stashes: one with 2 entries, one with 1 entry.
$node->safe_psql("postgres", qq{
	SELECT pg_create_advice_stash('stash_a');
	SELECT pg_set_stashed_advice('stash_a', 1001, 'IndexScan(t)');
	SELECT pg_set_stashed_advice('stash_a', 1002, E'line1\\nline2\\ttab\\\\backslash');
	SELECT pg_create_advice_stash('stash_b');
	SELECT pg_set_stashed_advice('stash_b', 2001, 'SeqScan(t)');
});

# Verify before restart.
my $result = $node->safe_psql("postgres",
	"SELECT stash_name, num_entries FROM pg_get_advice_stashes() ORDER BY stash_name");
is($result, "stash_a|2\nstash_b|1", 'stashes present before restart');

# Restart and verify the data survived.
$node->restart;
$node->wait_for_log("loaded 2 advice stashes and 3 entries");

$result = $node->safe_psql("postgres",
	"SELECT stash_name, num_entries FROM pg_get_advice_stashes() ORDER BY stash_name");
is($result, "stash_a|2\nstash_b|1", 'stashes survived restart');

# Verify entry contents, including the one with special characters.
$result = $node->safe_psql("postgres",
	"SELECT stash_name, query_id, advice_string FROM pg_get_advice_stash_contents(NULL) ORDER BY stash_name, query_id");
is($result,
	"stash_a|1001|IndexScan(t)\nstash_a|1002|line1\nline2\ttab\\backslash\nstash_b|2001|SeqScan(t)",
	'entry contents survived restart with special characters intact');

# Add a third stash with 0 entries.
$node->safe_psql("postgres", qq{
	SELECT pg_create_advice_stash('stash_c');
});

# Restart again and verify all three stashes are present.
$node->restart;
$node->wait_for_log("loaded 3 advice stashes and 3 entries");

$result = $node->safe_psql("postgres",
	"SELECT stash_name, num_entries FROM pg_get_advice_stashes() ORDER BY stash_name");
is($result, "stash_a|2\nstash_b|1\nstash_c|0", 'all three stashes survived second restart');

# Drop all stashes and verify the dump file is removed after restart.
$node->safe_psql("postgres", qq{
	SELECT pg_drop_advice_stash('stash_a');
	SELECT pg_drop_advice_stash('stash_b');
	SELECT pg_drop_advice_stash('stash_c');
});

$node->restart;

$result = $node->safe_psql("postgres",
	"SELECT count(*) FROM pg_get_advice_stashes()");
is($result, "0", 'no stashes after dropping all and restarting');

ok(!-f $node->data_dir . '/pg_stash_advice.tsv',
	'dump file removed after all stashes dropped');

$node->stop;

done_testing();
