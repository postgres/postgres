
# Copyright (c) 2026, PostgreSQL Global Development Group

# Test suite for testing enabling data checksums in an online cluster with
# injection point tests injecting failures into the processing

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use DataChecksums::Utils;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

# ---------------------------------------------------------------------------
# Test cluster setup
#

# Initiate test cluster
my $node = PostgreSQL::Test::Cluster->new('injection_node');
$node->init(no_data_checksums => 1);
$node->start;

# Set up test environment
$node->safe_psql('postgres', 'CREATE EXTENSION test_checksums;');
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points;');

# ---------------------------------------------------------------------------
# Inducing failures and crashes in processing

# Force enabling checksums to fail by marking one of the databases as having
# failed in processing.
disable_data_checksums($node, wait => 1);
$node->safe_psql('postgres',
	"SELECT injection_points_attach('datachecksumsworker-fail-db-result','notice');"
);
enable_data_checksums($node, wait => 'off');
$node->safe_psql('postgres',
	"SELECT injection_points_detach('datachecksumsworker-fail-db-result');");

# Make sure that disabling after a failure works
disable_data_checksums($node);
test_checksum_state($node, 'off');

# ---------------------------------------------------------------------------
# Timing and retry related tests
#

SKIP:
{
	skip 'Data checksum delay tests not enabled in PG_TEST_EXTRA', 4
	  if (!$ENV{PG_TEST_EXTRA}
		|| $ENV{PG_TEST_EXTRA} !~ /\bchecksum_extended\b/);

	# Inject a delay in the barrier for enabling checksums
	disable_data_checksums($node, wait => 1);
	$node->safe_psql('postgres', 'SELECT dcw_inject_delay_barrier();');
	enable_data_checksums($node, wait => 'on');

	# Fake the existence of a temporary table at the start of processing, which
	# will force the processing to wait and retry in order to wait for it to
	# disappear.
	disable_data_checksums($node, wait => 1);
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('datachecksumsworker-fake-temptable-wait', 'notice');"
	);
	enable_data_checksums($node, wait => 'on');
	$node->safe_psql('postgres',
		"SELECT injection_points_detach('datachecksumsworker-fake-temptable-wait');"
	);
}

# ---------------------------------------------------------------------------
# Test an interrupted DROP DATABASE while checksum enabling is in progress
#

disable_data_checksums($node, wait => 1);

$node->safe_psql('postgres', 'CREATE DATABASE invalid_dropdb;');
$node->safe_psql('invalid_dropdb',
	"CREATE TABLE bad_t AS SELECT generate_series(1,1000) AS a;");

# Hold the worker in "postgres", so invalid_dropdb is still waiting in the
# launcher's database list when DROP DATABASE is interrupted.
my $hold = $node->background_psql('postgres');
$hold->query_safe('CREATE TEMP TABLE holdme (a int);');

my $dropdb_log_offset = -s $node->logfile;
enable_data_checksums($node);
$node->poll_query_until(
	'postgres', qq[
	SELECT count(*) > 0 FROM pg_stat_activity
	WHERE backend_type = 'datachecksums worker' AND datname = 'postgres'
	  AND query LIKE 'Waiting for % temp tables to be removed']
) or die "timed out waiting for worker to wait for temporary tables";

my $dropdb_log =
  PostgreSQL::Test::Utils::slurp_file($node->logfile, $dropdb_log_offset);
unlike(
	$dropdb_log,
	qr/initiating data checksum processing in database "invalid_dropdb"/,
	'invalid database has not been processed yet');

# Leave the database durably marked invalid, but abort DROP DATABASE before
# its catalog row and files are removed.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('dropdb-after-invalid-marker','error');");
my ($drop_ret, $drop_stdout, $drop_stderr) =
  $node->psql('postgres', 'DROP DATABASE invalid_dropdb;');
isnt($drop_ret, 0, 'DROP DATABASE was interrupted after invalidation');
like(
	$drop_stderr,
	qr/dropdb-after-invalid-marker/,
	'DROP DATABASE reached the invalid-marker injection point');
$node->safe_psql('postgres',
	"SELECT injection_points_detach('dropdb-after-invalid-marker');");

my $invalid_state = $node->safe_psql('postgres',
	"SELECT datconnlimit FROM pg_database WHERE datname = 'invalid_dropdb';");
is($invalid_state, '-2', 'interrupted DROP left an invalid database row');

# Let checksum processing continue.  The invalid database must be treated as
# a processing failure, not as a successfully dropped database.
$hold->query_safe('DROP TABLE holdme;');
$hold->quit;
$node->poll_query_until('postgres',
		"SELECT count(*) = 0 "
	  . "FROM pg_catalog.pg_stat_activity "
	  . "WHERE backend_type = 'datachecksums launcher';")
  or die "timed out waiting for datachecksums launcher to exit";
test_checksum_state($node, 'off');

# Remove the invalid database and continue with the remaining tests.
$node->safe_psql('postgres', 'DROP DATABASE invalid_dropdb;');

# ---------------------------------------------------------------------------
# Test concurrent CREATE DATABASE which use the file_copy strategy
#

disable_data_checksums($node, wait => 1);
my $node_loglocation = -s $node->logfile;

$node->safe_psql('postgres',
	"CREATE TABLE t AS SELECT generate_series(1,10000) AS a;");

$node->safe_psql('postgres',
	"SELECT injection_points_attach('createdb-before-catalog-insert','wait');"
);
$node->safe_psql('postgres',
	"SELECT injection_points_attach('datachecksumsworker-fake-temptable-wait','wait');"
);

# Hold CREATE DATABASE after the strategy check, before its xact is visible.
my $bg = $node->background_psql('postgres');
$bg->query_until(
	qr/starting_create/, q(
\echo starting_create
CREATE DATABASE fcdb TEMPLATE template0 STRATEGY file_copy;
));
$node->wait_for_event('client backend', 'createdb-before-catalog-insert');

# Enable checksums, worker holds before processing template0.
enable_data_checksums($node);
$node->wait_for_event('datachecksums worker',
	'datachecksumsworker-fake-temptable-wait');

# Release CREATE DATABASE, must fail on the recheck instead of raw-copying.
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('createdb-before-catalog-insert');");
$node->safe_psql('postgres',
	"SELECT injection_points_detach('createdb-before-catalog-insert');");

# Wait for the CREATE DATABASE xact to finish before releasing the worker.
$node->poll_query_until('postgres',
		"SELECT count(*) = 0 FROM pg_catalog.pg_stat_activity "
	  . "WHERE query LIKE 'CREATE DATABASE%' AND state != 'idle';");

$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('datachecksumsworker-fake-temptable-wait');"
);
$node->safe_psql('postgres',
	"SELECT injection_points_detach('datachecksumsworker-fake-temptable-wait');"
);

wait_for_checksum_state($node, 'on');

my $result = $node->safe_psql('postgres',
	"SELECT count(*) FROM pg_catalog.pg_database WHERE datname = 'fcdb';");
is($result, '0', 'file_copy database creation was refused');

my $log =
  PostgreSQL::Test::Utils::slurp_file($node->logfile, $node_loglocation);
like(
	$log,
	qr/create database strategy "file_copy" not allowed/m,
	'file_copy error message in log');

# ---------------------------------------------------------------------------
# Test teardown
#

$bg->{run}->finish;
$bg->quit;
$node->stop;
done_testing();
