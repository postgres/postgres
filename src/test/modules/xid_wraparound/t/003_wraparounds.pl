# Copyright (c) 2023-2025, PostgreSQL Global Development Group
#
# Consume a lot of XIDs, wrapping around a few times.
#

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

if (!$ENV{PG_TEST_EXTRA} || $ENV{PG_TEST_EXTRA} !~ /\bxid_wraparound\b/)
{
	plan skip_all => "test xid_wraparound not enabled in PG_TEST_EXTRA";
}

# Initialize node
my $node = PostgreSQL::Test::Cluster->new('wraparound');

$node->init;
$node->append_conf(
	'postgresql.conf', qq[
autovacuum_naptime = 1s
# so it's easier to verify the order of operations
autovacuum_max_workers = 1
log_autovacuum_min_duration = 0
]);
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION xid_wraparound');

# Create a test table. We disable autovacuum on the table to run
# it only to prevent wraparound.
$node->safe_psql(
	'postgres', qq[
CREATE TABLE wraparoundtest(t text) WITH (autovacuum_enabled = off);
INSERT INTO wraparoundtest VALUES ('beginning');
]);

# Bump the query timeout to avoid false negatives on slow test systems.
my $psql_timeout_secs = 4 * $PostgreSQL::Test::Utils::timeout_default;

# Burn through 10 billion transactions in total, in batches of 100 million.
my $ret;
for my $i (1 .. 100)
{
	$ret = $node->safe_psql(
		'postgres',
		qq[SELECT consume_xids(100000000)],
		timeout => $psql_timeout_secs);
	$ret = $node->safe_psql('postgres',
		qq[INSERT INTO wraparoundtest VALUES ('after $i batches')]);
}

$ret = $node->safe_psql('postgres', qq[SELECT COUNT(*) FROM wraparoundtest]);
is($ret, "101");

$node->stop;

done_testing();
