
# Copyright (c) 2026, PostgreSQL Global Development Group

# Test suite for testing enabling data checksums in an online cluster with a
# restart which breaks processing.
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use DataChecksums::Utils;

# Initialize node with checksums disabled.
my $node = PostgreSQL::Test::Cluster->new('restarts_node');
$node->init(no_data_checksums => 1);
$node->start;

# Initialize result storage for queries
my $result;

# Create some content to have un-checksummed data in the cluster
$node->safe_psql('postgres',
	"CREATE TABLE t AS SELECT generate_series(1,10000) AS a;");

# Ensure that checksums are disabled
test_checksum_state($node, 'off');

SKIP:
{
	skip 'Data checksum delay tests not enabled in PG_TEST_EXTRA', 6
	  if (!$ENV{PG_TEST_EXTRA}
		|| $ENV{PG_TEST_EXTRA} !~ /\bchecksum_extended\b/);

	# Create a barrier for checksum enablement to block on, in this case a pre-
	# existing temporary table which is kept open while processing is started.
	# We can accomplish this by setting up an interactive psql process which
	# keeps the temporary table created as we enable checksums in another psql
	# process.
	#
	# This is a similar test to the synthetic variant in 005_injection.pl
	# which fakes this scenario.
	my $bsession = $node->background_psql('postgres');
	$bsession->query_safe('CREATE TEMPORARY TABLE tt (a integer);');

	# In another session, make sure we can see the blocking temp table but
	# start processing anyways and check that we are blocked with a proper
	# wait event.
	$result = $node->safe_psql('postgres',
		"SELECT relpersistence FROM pg_catalog.pg_class WHERE relname = 'tt';"
	);
	is($result, 't', 'ensure we can see the temporary table');

	# Enabling data checksums shouldn't work as the process is blocked on the
	# temporary table held open by $bsession. Ensure that we reach inprogress-
	# on before we do more tests.
	enable_data_checksums($node, wait => 'inprogress-on');

	# Wait for processing to finish and the worker waiting for leftover temp
	# relations to be able to actually finish
	$result = $node->poll_query_until(
		'postgres',
		"SELECT wait_event FROM pg_catalog.pg_stat_activity "
		  . "WHERE backend_type = 'datachecksum worker';",
		'ChecksumEnableTemptableWait');

	# The datachecksumsworker waits for temporary tables to disappear for 3
	# seconds before retrying, so sleep for 4 seconds to be guaranteed to see
	# a retry cycle
	sleep(4);

	# Re-check the wait event to ensure we are blocked on the right thing.
	$result = $node->safe_psql('postgres',
			"SELECT wait_event FROM pg_catalog.pg_stat_activity "
		  . "WHERE backend_type = 'datachecksum worker';");
	is($result, 'ChecksumEnableTemptableWait',
		'ensure the correct wait condition is set');
	test_checksum_state($node, 'inprogress-on');

	# Stop the cluster while bsession is still attached.  We can't close the
	# session first since the brief period between closing and stopping might
	# be enough for checksums to get enabled.
	$node->stop;
	$bsession->quit;
	$node->start;

	# Ensure the checksums aren't enabled across the restart.  This leaves the
	# cluster in the same state as before we entered the SKIP block.
	test_checksum_state($node, 'off');
}

enable_data_checksums($node, wait => 'on');

$result = $node->safe_psql('postgres', "SELECT count(*) FROM t WHERE a > 1");
is($result, '9999', 'ensure checksummed pages can be read back');

$result = $node->poll_query_until(
	'postgres',
	"SELECT count(*) FROM pg_stat_activity WHERE backend_type LIKE 'datachecksum%';",
	'0');
is($result, 1, 'await datachecksums worker/launcher termination');

disable_data_checksums($node, wait => 1);

$node->stop;
done_testing();
