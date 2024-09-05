
# Copyright (c) 2024, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use locale;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Time::HiRes qw(usleep);
use Test::More;

# Test timeouts that will cause FATAL errors.  This test relies on injection
# points to await a timeout occurrence. Relying on sleep proved to be unstable
# on buildfarm. It's difficult to rely on the NOTICE injection point because
# the backend under FATAL error can behave differently.

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

# Node initialization
my $node = PostgreSQL::Test::Cluster->new('master');
$node->init();
$node->start;

# Check if the extension injection_points is available, as it may be
# possible that this script is run with installcheck, where the module
# would not be installed by default.
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points;');

#
# 1. Test of the transaction timeout
#

$node->safe_psql('postgres',
	"SELECT injection_points_attach('transaction-timeout', 'wait');");

my $psql_session = $node->background_psql('postgres');

# The following query will generate a stream of SELECT 1 queries. This is done
# so to exercise transaction timeout in the presence of short queries.
# Note: the interval value is parsed with locale-aware strtod()
$psql_session->query_until(
	qr/starting_bg_psql/,
	sprintf(
		q(\echo starting_bg_psql
		SET transaction_timeout to '10ms';
		BEGIN;
		SELECT 1 \watch %g
		\q
), 0.001));

# Wait until the backend enters the timeout injection point. Will get an error
# here if anything goes wrong.
$node->wait_for_event('client backend', 'transaction-timeout');

my $log_offset = -s $node->logfile;

# Remove the injection point.
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('transaction-timeout');");

# Check that the timeout was logged.
$node->wait_for_log('terminating connection due to transaction timeout',
	$log_offset);

# If we send \q with $psql_session->quit the command can be sent to the session
# already closed. So \q is in initial script, here we only finish IPC::Run.
$psql_session->{run}->finish;


#
# 2. Test of the idle in transaction timeout
#

$node->safe_psql('postgres',
	"SELECT injection_points_attach('idle-in-transaction-session-timeout', 'wait');"
);

# We begin a transaction and the hand on the line
$psql_session = $node->background_psql('postgres');
$psql_session->query_until(
	qr/starting_bg_psql/, q(
   \echo starting_bg_psql
   SET idle_in_transaction_session_timeout to '10ms';
   BEGIN;
));

# Wait until the backend enters the timeout injection point.
$node->wait_for_event('client backend',
	'idle-in-transaction-session-timeout');

$log_offset = -s $node->logfile;

# Remove the injection point.
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('idle-in-transaction-session-timeout');");

# Check that the timeout was logged.
$node->wait_for_log(
	'terminating connection due to idle-in-transaction timeout', $log_offset);

ok($psql_session->quit);


#
# 3. Test of the idle session timeout
#
$node->safe_psql('postgres',
	"SELECT injection_points_attach('idle-session-timeout', 'wait');");

# We just initialize the GUC and wait. No transaction is required.
$psql_session = $node->background_psql('postgres');
$psql_session->query_until(
	qr/starting_bg_psql/, q(
   \echo starting_bg_psql
   SET idle_session_timeout to '10ms';
));

# Wait until the backend enters the timeout injection point.
$node->wait_for_event('client backend', 'idle-session-timeout');

$log_offset = -s $node->logfile;

# Remove the injection point.
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('idle-session-timeout');");

# Check that the timeout was logged.
$node->wait_for_log('terminating connection due to idle-session timeout',
	$log_offset);

ok($psql_session->quit);

done_testing();
