
# Copyright (c) 2026, PostgreSQL Global Development Group

# Test suite for testing enabling data checksums in an online cluster
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use DataChecksums::Utils;

# Initialize node with checksums enabled to test pg_control_init returning
# 1 for this cluster, the remaining tests will initialize to off to test the
# return value for a cluster initialized without checksums
my $node = PostgreSQL::Test::Cluster->new('basic_node');
$node->init;
$node->start;

# Create some content to have data in the cluster
$node->safe_psql('postgres',
	"CREATE TABLE t AS SELECT generate_series(1,10000) AS a;");

# Ensure that checksums are turned on
test_checksum_state($node, 'on');

# Disable data checksums and wait for the state transition to 'off'
disable_data_checksums($node, wait => 'off');

# Make sure pg_control_init reports the initial enabled state
my $result = $node->safe_psql('postgres',
	'SELECT data_page_checksum_version FROM pg_control_init();');
is($result, '1', 'ensure pg_control_init reports enabled state');

# Enable data checksums and wait for the state transition to 'on'
enable_data_checksums($node, wait => 'on');

# Run a dummy query just to make sure we can read back data
$result = $node->safe_psql('postgres', "SELECT count(*) FROM t WHERE a > 1 ");
is($result, '9999', 'ensure checksummed pages can be read back');

# Enable data checksums again which should be a no-op so we explicitly don't
# wait for any state transition as none should happen here.
enable_data_checksums($node);
test_checksum_state($node, 'on');
# ..and make sure we can still read/write data
$node->safe_psql('postgres', "UPDATE t SET a = a + 1;");
$result = $node->safe_psql('postgres', "SELECT count(*) FROM t WHERE a > 1");
is($result, '10000', 'ensure checksummed pages can be read back');

# Disable checksums again and wait for the state transition
disable_data_checksums($node, wait => 1);

# Test reading data again
$result = $node->safe_psql('postgres', "SELECT count(*) FROM t WHERE a > 1");
is($result, '10000', 'ensure previously checksummed pages can be read back');

# Re-enable checksums and make sure that the underlying data has changed to
# ensure that checksums will be different.
$node->safe_psql('postgres', "UPDATE t SET a = a + 1;");
enable_data_checksums($node, wait => 'on');

# Run a dummy query just to make sure we can read back the data
$result = $node->safe_psql('postgres', "SELECT count(*) FROM t WHERE a > 1");
is($result, '10000', 'ensure checksummed pages can be read back');

# Enabling checksums in a cluster which contains an invalid database left
# behind by an interrupted DROP DATABASE must be refused.
disable_data_checksums($node, wait => 1);

$node->safe_psql('postgres', "CREATE DATABASE baddb;");
$node->safe_psql('baddb',
	"CREATE TABLE bad_t AS SELECT generate_series(1,100) AS a;");

# Mark the database invalid, as an interrupted DROP DATABASE would.
$node->safe_psql('postgres',
	"UPDATE pg_database SET datconnlimit = -2 WHERE datname = 'baddb';");

# The request must fail up front with an actionable error, rather than fail
# halfway through processing.
my ($ret, $stdout, $stderr) =
  $node->psql('postgres', "SELECT pg_enable_data_checksums();");
isnt($ret, 0, 'pg_enable_data_checksums fails with an invalid database');
like(
	$stderr,
	qr/invalid database "baddb"/,
	'error message names the invalid database');
like(
	$stderr,
	qr/DROP DATABASE/,
	'error message hints at dropping the database');
test_checksum_state($node, 'off');

# Dropping the invalid database clears the way.
$node->safe_psql('postgres', "DROP DATABASE baddb;");
enable_data_checksums($node, wait => 'on');

# A database dropped while processing is in progress is not an error, the
# remaining databases are still processed.
disable_data_checksums($node, wait => 1);

$node->safe_psql('postgres', "CREATE DATABASE dropme;");
$node->safe_psql('dropme',
	"CREATE TABLE dropme_t AS SELECT generate_series(1,10000) AS a;");

# Hold the worker in the "postgres" database by keeping a temporary table
# around, the worker waits for pre-existing temp tables to disappear before
# it reports the database as processed.  "dropme" was created last, so it is
# processed after "postgres" and is still untouched while we wait.
my $bg = $node->background_psql('postgres');
$bg->query_safe('CREATE TEMP TABLE holdme (a int);');

enable_data_checksums($node);

$node->poll_query_until(
	'postgres', qq[
	SELECT count(*) > 0 FROM pg_stat_activity
	WHERE backend_type = 'datachecksums worker' AND datname = 'postgres'
	  AND query LIKE 'Waiting for % temp tables to be removed']
) or die "timed out waiting for worker to wait for temporary tables";

# Verify the assumption that processing has not reached "dropme" yet, without
# it the test would silently stop covering the concurrent drop.
my $log = slurp_file($node->logfile);
unlike(
	$log,
	qr/initiating data checksum processing in database "dropme"/,
	'processing has not reached the database to drop');

# Not processed yet and nobody is connected to it, so this must succeed.
$node->safe_psql('postgres', "DROP DATABASE dropme;");

# Let the worker in "postgres" finish, the launcher then moves on to the
# database which no longer exists.
$bg->query_safe('DROP TABLE holdme;');
$bg->quit;

wait_for_checksum_state($node, 'on');
$node->poll_query_until('postgres',
		"SELECT count(*) = 0 "
	  . "FROM pg_catalog.pg_stat_activity "
	  . "WHERE backend_type = 'datachecksums launcher';");

# Same thing with DROP DATABASE ... WITH (FORCE), which terminates the
# checksums worker connected to the database being dropped.
disable_data_checksums($node, wait => 1);

$node->safe_psql('postgres', "CREATE DATABASE dropmeforce;");
$node->safe_psql('dropmeforce',
	"CREATE TABLE dropme_t AS SELECT generate_series(1,10000) AS a;");

# Hold the worker inside "dropmeforce" by keeping a temporary table around
# there.
$bg = $node->background_psql('dropmeforce');
$bg->query_safe('CREATE TEMP TABLE holdme (a int);');

enable_data_checksums($node);

$node->poll_query_until(
	'postgres', qq[
	SELECT count(*) > 0 FROM pg_stat_activity
	WHERE backend_type = 'datachecksums worker' AND datname = 'dropmeforce'
	  AND query LIKE 'Waiting for % temp tables to be removed']
) or die "timed out waiting for worker to wait for temporary tables";

# Terminates both the session holding the temp table and the checksums
# worker connected to the database.
$node->safe_psql('postgres', "DROP DATABASE dropmeforce WITH (FORCE);");
$bg->{run}->finish;
$bg->quit;

wait_for_checksum_state($node, 'on');
$node->poll_query_until('postgres',
		"SELECT count(*) = 0 "
	  . "FROM pg_catalog.pg_stat_activity "
	  . "WHERE backend_type = 'datachecksums launcher';");

$result = $node->safe_psql('postgres', "SELECT count(*) FROM t WHERE a > 1");
is($result, '10000', 'ensure checksummed pages can be read back');

$node->stop;

# The resulting cluster must also pass offline verification, proving no
# unchecksummed files were left behind.
command_ok(
	[ 'pg_checksums', '--check', '-D', $node->data_dir ],
	'offline checksum verification passes after enable');

done_testing();
