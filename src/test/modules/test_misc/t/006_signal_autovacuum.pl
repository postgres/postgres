# Copyright (c) 2024, PostgreSQL Global Development Group

# Test signaling autovacuum worker with pg_signal_autovacuum_worker.
#
# Only roles with privileges of pg_signal_autovacuum_worker are allowed to
# signal autovacuum workers.  This test uses an injection point located
# at the beginning of the autovacuum worker startup.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

# Initialize postgres
my $psql_err = '';
my $psql_out = '';
my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;

# This ensures a quick worker spawn.
$node->append_conf('postgresql.conf', 'autovacuum_naptime = 1');
$node->start;

# Check if the extension injection_points is available, as it may be
# possible that this script is run with installcheck, where the module
# would not be installed by default.
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points;');

$node->safe_psql(
	'postgres', qq(
    CREATE ROLE regress_regular_role;
    CREATE ROLE regress_worker_role;
    GRANT pg_signal_autovacuum_worker TO regress_worker_role;
));

# From this point, autovacuum worker will wait at startup.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('autovacuum-worker-start', 'wait');");

# Accelerate worker creation in case we reach this point before the naptime
# ends.
$node->reload();

# Wait until an autovacuum worker starts.
$node->wait_for_event('autovacuum worker', 'autovacuum-worker-start');

# And grab one of them.
my $av_pid = $node->safe_psql(
	'postgres', qq(
    SELECT pid FROM pg_stat_activity WHERE backend_type = 'autovacuum worker' AND wait_event = 'autovacuum-worker-start' LIMIT 1;
));

# Regular role cannot terminate autovacuum worker.
my $terminate_with_no_pg_signal_av = $node->psql(
	'postgres', qq(
    SET ROLE regress_regular_role;
    SELECT pg_terminate_backend('$av_pid');
),
	stdout => \$psql_out,
	stderr => \$psql_err);

like(
	$psql_err,
	qr/ERROR:  permission denied to terminate process\nDETAIL:  Only roles with privileges of the "pg_signal_autovacuum_worker" role may terminate autovacuum workers./,
	"autovacuum worker not signaled with regular role");

my $offset = -s $node->logfile;

# Role with pg_signal_autovacuum_worker can terminate autovacuum worker.
my $terminate_with_pg_signal_av = $node->psql(
	'postgres', qq(
    SET ROLE regress_worker_role;
    SELECT pg_terminate_backend('$av_pid');
),
	stdout => \$psql_out,
	stderr => \$psql_err);

# Wait for the autovacuum worker to exit before scanning the logs.
$node->poll_query_until('postgres',
		"SELECT count(*) = 0 FROM pg_stat_activity "
	  . "WHERE pid = '$av_pid' AND backend_type = 'autovacuum worker';");

# Check that the primary server logs a FATAL indicating that autovacuum
# is terminated.
ok( $node->log_contains(
		qr/FATAL: .*terminating autovacuum process due to administrator command/,
		$offset),
	"autovacuum worker signaled with pg_signal_autovacuum_worker granted");

# Release injection point.
$node->safe_psql('postgres',
	"SELECT injection_points_detach('autovacuum-worker-start');");

done_testing();
