# Copyright (c) 2023, PostgreSQL Global Development Group

# Test worker_spi module.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('mynode');
$node->init;
$node->start;

note "testing dynamic bgworkers";

$node->safe_psql('postgres', 'CREATE EXTENSION worker_spi;');

# Launch one dynamic worker, then wait for its initialization to complete.
# This consists in making sure that a table name "counted" is created
# on a new schema whose name includes the index defined in input argument
# of worker_spi_launch().
# By default, dynamic bgworkers connect to the "postgres" database.
my $result =
  $node->safe_psql('postgres', 'SELECT worker_spi_launch(4) IS NOT NULL;');
is($result, 't', "dynamic bgworker launched");
$node->poll_query_until(
	'postgres',
	qq[SELECT count(*) > 0 FROM information_schema.tables
	    WHERE table_schema = 'schema4' AND table_name = 'counted';]);
$node->safe_psql('postgres',
	"INSERT INTO schema4.counted VALUES ('total', 0), ('delta', 1);");
# Issue a SIGHUP on the node to force the worker to loop once, accelerating
# this test.
$node->reload;
# Wait until the worker has processed the tuple that has just been inserted.
$node->poll_query_until('postgres',
	qq[SELECT count(*) FROM schema4.counted WHERE type = 'delta';], '0');
$result = $node->safe_psql('postgres', 'SELECT * FROM schema4.counted;');
is($result, qq(total|1), 'dynamic bgworker correctly consumed tuple data');

# Check the wait event used by the dynamic bgworker.
$result = $node->poll_query_until(
	'postgres',
	qq[SELECT wait_event FROM pg_stat_activity WHERE backend_type ~ 'worker_spi';],
	qq[worker_spi_main]);
is($result, 1,
	'dynamic bgworker has reported "worker_spi_main" as wait event');

# Check the wait event used by the dynamic bgworker appears in pg_wait_events
$result = $node->safe_psql('postgres',
	q[SELECT count(*) > 0 from pg_wait_events where type = 'Extension' and name = 'worker_spi_main';]
);
is($result, 't', '"worker_spi_main" is reported in pg_wait_events');

note "testing bgworkers loaded with shared_preload_libraries";

# Create the database first so as the workers can connect to it when
# the library is loaded.
$node->safe_psql('postgres', q(CREATE DATABASE mydb;));
$node->safe_psql('mydb', 'CREATE EXTENSION worker_spi;');

# Now load the module as a shared library.
$node->append_conf(
	'postgresql.conf', q{
shared_preload_libraries = 'worker_spi'
worker_spi.database = 'mydb'
worker_spi.total_workers = 3
});
$node->restart;

# Check that bgworkers have been registered and launched.
ok( $node->poll_query_until(
		'mydb',
		qq[SELECT datname, count(datname), wait_event FROM pg_stat_activity
            WHERE backend_type = 'worker_spi' GROUP BY datname, wait_event;],
		'mydb|3|worker_spi_main'),
	'bgworkers all launched'
) or die "Timed out while waiting for bgworkers to be launched";

# Ask worker_spi to launch dynamic bgworkers with the library loaded, then
# check their existence.  Use IDs that do not overlap with the schemas created
# by the previous workers.
my $worker1_pid = $node->safe_psql('mydb', 'SELECT worker_spi_launch(10);');
my $worker2_pid = $node->safe_psql('mydb', 'SELECT worker_spi_launch(11);');

ok( $node->poll_query_until(
		'mydb',
		qq[SELECT datname, count(datname), wait_event FROM pg_stat_activity
            WHERE backend_type = 'worker_spi dynamic' AND
            pid IN ($worker1_pid, $worker2_pid) GROUP BY datname, wait_event;],
		'mydb|2|worker_spi_main'),
	'dynamic bgworkers all launched'
) or die "Timed out while waiting for dynamic bgworkers to be launched";

done_testing();
