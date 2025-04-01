# Copyright (c) 2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use List::Util qw(shuffle);


my $node = PostgreSQL::Test::Cluster->new('worker');
$node->init();
$node->append_conf(
	'postgresql.conf', qq(
io_method=worker
));

$node->start();

# Test changing the number of I/O worker processes while also evaluating the
# handling of their termination.
test_number_of_io_workers_dynamic($node);

$node->stop();

done_testing();


sub test_number_of_io_workers_dynamic
{
	my $node = shift;

	my $prev_worker_count = $node->safe_psql('postgres', 'SHOW io_workers');

	# Verify that worker count can't be set to 0
	change_number_of_io_workers($node, 0, $prev_worker_count, 1);

	# Verify that worker count can't be set to 33 (above the max)
	change_number_of_io_workers($node, 33, $prev_worker_count, 1);

	# Try changing IO workers to a random value and verify that the worker
	# count ends up as expected. Always test the min/max of workers.
	#
	# Valid range for io_workers is [1, 32]. 8 tests in total seems
	# reasonable.
	my @io_workers_range = shuffle(1 ... 32);
	foreach my $worker_count (1, 32, @io_workers_range[ 0, 6 ])
	{
		$prev_worker_count =
		  change_number_of_io_workers($node, $worker_count,
			$prev_worker_count, 0);
	}
}

sub change_number_of_io_workers
{
	my $node = shift;
	my $worker_count = shift;
	my $prev_worker_count = shift;
	my $expect_failure = shift;
	my ($result, $stdout, $stderr);

	($result, $stdout, $stderr) =
	  $node->psql('postgres', "ALTER SYSTEM SET io_workers = $worker_count");
	$node->safe_psql('postgres', 'SELECT pg_reload_conf()');

	if ($expect_failure)
	{
		ok( $stderr =~
			  /$worker_count is outside the valid range for parameter "io_workers"/,
			"updating number of io_workers to $worker_count failed, as expected"
		);

		return $prev_worker_count;
	}
	else
	{
		is( $node->safe_psql('postgres', 'SHOW io_workers'),
			$worker_count,
			"updating number of io_workers from $prev_worker_count to $worker_count"
		);

		check_io_worker_count($node, $worker_count);
		terminate_io_worker($node, $worker_count);
		check_io_worker_count($node, $worker_count);

		return $worker_count;
	}
}

sub terminate_io_worker
{
	my $node = shift;
	my $worker_count = shift;
	my ($pid, $ret);

	# Select a random io worker
	$pid = $node->safe_psql(
		'postgres',
		qq(SELECT pid FROM pg_stat_activity WHERE
			backend_type = 'io worker' ORDER BY RANDOM() LIMIT 1));

	# terminate IO worker with SIGINT
	is(PostgreSQL::Test::Utils::system_log('pg_ctl', 'kill', 'INT', $pid),
		0, "random io worker process signalled with INT");

	# Check that worker exits
	ok( $node->poll_query_until(
			'postgres',
			qq(SELECT COUNT(*) FROM pg_stat_activity WHERE pid = $pid), '0'),
		"random io worker process exited after signal");
}

sub check_io_worker_count
{
	my $node = shift;
	my $worker_count = shift;

	ok( $node->poll_query_until(
			'postgres',
			qq(SELECT COUNT(*) FROM pg_stat_activity WHERE backend_type = 'io worker'),
			$worker_count),
		"io worker count is $worker_count");
}
