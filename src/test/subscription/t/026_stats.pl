
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Tests for subscription stats.
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Create publisher node.
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

# Create subscriber node.
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init(allows_streaming => 'logical');
$node_subscriber->start;


sub create_sub_pub_w_errors
{
	my ($node_publisher, $node_subscriber, $db, $table_name) = @_;
	# Initial table setup on both publisher and subscriber. On subscriber we
	# create the same tables but with primary keys. Also, insert some data that
	# will conflict with the data replicated from publisher later.
	$node_publisher->safe_psql(
		$db,
		qq[
	BEGIN;
	CREATE TABLE $table_name(a int);
	INSERT INTO $table_name VALUES (1);
	COMMIT;
	]);
	$node_subscriber->safe_psql(
		$db,
		qq[
	BEGIN;
	CREATE TABLE $table_name(a int primary key);
	INSERT INTO $table_name VALUES (1);
	COMMIT;
	]);

	# Set up publication.
	my $pub_name          = $table_name . '_pub';
	my $publisher_connstr = $node_publisher->connstr . qq( dbname=$db);

	$node_publisher->safe_psql($db,
		qq(CREATE PUBLICATION $pub_name FOR TABLE $table_name));

	# Create subscription. The tablesync for table on subscription will enter into
	# infinite error loop due to violating the unique constraint.
	my $sub_name = $table_name . '_sub';
	$node_subscriber->safe_psql($db,
		qq(CREATE SUBSCRIPTION $sub_name CONNECTION '$publisher_connstr' PUBLICATION $pub_name)
	);

	$node_publisher->wait_for_catchup($sub_name);

	# Wait for the tablesync error to be reported.
	$node_subscriber->poll_query_until(
		$db,
		qq[
	SELECT sync_error_count > 0
	FROM pg_stat_subscription_stats
	WHERE subname = '$sub_name'
	])
	  or die
	  qq(Timed out while waiting for tablesync errors for subscription '$sub_name');

	# Truncate test_tab1 so that tablesync worker can continue.
	$node_subscriber->safe_psql($db, qq(TRUNCATE $table_name));

	# Wait for initial tablesync to finish.
	$node_subscriber->poll_query_until(
		$db,
		qq[
	SELECT count(1) = 1 FROM pg_subscription_rel
	WHERE srrelid = '$table_name'::regclass AND srsubstate in ('r', 's')
	])
	  or die
	  qq(Timed out while waiting for subscriber to synchronize data for table '$table_name'.);

	# Check test table on the subscriber has one row.
	my $result =
	  $node_subscriber->safe_psql($db, qq(SELECT a FROM $table_name));
	is($result, qq(1), qq(Check that table '$table_name' now has 1 row.));

	# Insert data to test table on the publisher, raising an error on the
	# subscriber due to violation of the unique constraint on test table.
	$node_publisher->safe_psql($db, qq(INSERT INTO $table_name VALUES (1)));

	# Wait for the apply error to be reported.
	$node_subscriber->poll_query_until(
		$db,
		qq[
	SELECT apply_error_count > 0
	FROM pg_stat_subscription_stats
	WHERE subname = '$sub_name'
	])
	  or die
	  qq(Timed out while waiting for apply error for subscription '$sub_name');

	# Truncate test table so that apply worker can continue.
	$node_subscriber->safe_psql($db, qq(TRUNCATE $table_name));

	return ($pub_name, $sub_name);
}

my $db = 'postgres';

# There shouldn't be any subscription errors before starting logical replication.
my $result = $node_subscriber->safe_psql($db,
	qq(SELECT count(1) FROM pg_stat_subscription_stats));
is($result, qq(0),
	'Check that there are no subscription errors before starting logical replication.'
);

# Create the publication and subscription with sync and apply errors
my $table1_name = 'test_tab1';
my ($pub1_name, $sub1_name) =
  create_sub_pub_w_errors($node_publisher, $node_subscriber, $db,
	$table1_name);

# Apply and Sync errors are > 0 and reset timestamp is NULL
is( $node_subscriber->safe_psql(
		$db,
		qq(SELECT apply_error_count > 0,
	sync_error_count > 0,
	stats_reset IS NULL
	FROM pg_stat_subscription_stats
	WHERE subname = '$sub1_name')
	),
	qq(t|t|t),
	qq(Check that apply errors and sync errors are both > 0 and stats_reset is NULL for subscription '$sub1_name'.)
);

# Reset a single subscription
$node_subscriber->safe_psql($db,
	qq(SELECT pg_stat_reset_subscription_stats((SELECT subid FROM pg_stat_subscription_stats WHERE subname = '$sub1_name')))
);

# Apply and Sync errors are 0 and stats reset is not NULL
is( $node_subscriber->safe_psql(
		$db,
		qq(SELECT apply_error_count = 0,
	sync_error_count = 0,
	stats_reset IS NOT NULL
	FROM pg_stat_subscription_stats
	WHERE subname = '$sub1_name')
	),
	qq(t|t|t),
	qq(Confirm that apply errors and sync errors are both 0 and stats_reset is not NULL after reset for subscription '$sub1_name'.)
);

# Get reset timestamp
my $reset_time1 = $node_subscriber->safe_psql($db,
	qq(SELECT stats_reset FROM pg_stat_subscription_stats WHERE subname = '$sub1_name')
);

# Reset single sub again
$node_subscriber->safe_psql(
	$db,
	qq(SELECT pg_stat_reset_subscription_stats((SELECT subid FROM
	pg_stat_subscription_stats WHERE subname = '$sub1_name')))
);

# check reset timestamp is newer after reset
is( $node_subscriber->safe_psql(
		$db,
		qq(SELECT stats_reset > '$reset_time1'::timestamptz FROM
	pg_stat_subscription_stats WHERE subname = '$sub1_name')
	),
	qq(t),
	qq(Check reset timestamp for '$sub1_name' is newer after second reset.));

# Make second subscription and publication
my $table2_name = 'test_tab2';
my ($pub2_name, $sub2_name) =
  create_sub_pub_w_errors($node_publisher, $node_subscriber, $db,
	$table2_name);

# Apply and Sync errors are > 0 and reset timestamp is NULL
is( $node_subscriber->safe_psql(
		$db,
		qq(SELECT apply_error_count > 0,
	sync_error_count > 0,
	stats_reset IS NULL
	FROM pg_stat_subscription_stats
	WHERE subname = '$sub2_name')
	),
	qq(t|t|t),
	qq(Confirm that apply errors and sync errors are both > 0 and stats_reset is NULL for sub '$sub2_name'.)
);

# Reset all subscriptions
$node_subscriber->safe_psql($db,
	qq(SELECT pg_stat_reset_subscription_stats(NULL)));

# Apply and Sync errors are 0 and stats reset is not NULL
is( $node_subscriber->safe_psql(
		$db,
		qq(SELECT apply_error_count = 0,
	sync_error_count = 0,
	stats_reset IS NOT NULL
	FROM pg_stat_subscription_stats
	WHERE subname = '$sub1_name')
	),
	qq(t|t|t),
	qq(Confirm that apply errors and sync errors are both 0 and stats_reset is not NULL for sub '$sub1_name' after reset.)
);

is( $node_subscriber->safe_psql(
		$db,
		qq(SELECT apply_error_count = 0,
	sync_error_count = 0,
	stats_reset IS NOT NULL
	FROM pg_stat_subscription_stats
	WHERE subname = '$sub2_name')
	),
	qq(t|t|t),
	qq(Confirm that apply errors and sync errors are both 0 and stats_reset is not NULL for sub '$sub2_name' after reset.)
);

$reset_time1 = $node_subscriber->safe_psql($db,
	qq(SELECT stats_reset FROM pg_stat_subscription_stats WHERE subname = '$sub1_name')
);
my $reset_time2 = $node_subscriber->safe_psql($db,
	qq(SELECT stats_reset FROM pg_stat_subscription_stats WHERE subname = '$sub2_name')
);

# Reset all subscriptions
$node_subscriber->safe_psql($db,
	qq(SELECT pg_stat_reset_subscription_stats(NULL)));

# check reset timestamp for sub1 is newer after reset
is( $node_subscriber->safe_psql(
		$db,
		qq(SELECT stats_reset > '$reset_time1'::timestamptz FROM
	pg_stat_subscription_stats WHERE subname = '$sub1_name')
	),
	qq(t),
	qq(Confirm that reset timestamp for '$sub1_name' is newer after second reset.)
);

# check reset timestamp for sub2 is newer after reset
is( $node_subscriber->safe_psql(
		$db,
		qq(SELECT stats_reset > '$reset_time2'::timestamptz FROM
	pg_stat_subscription_stats WHERE subname = '$sub2_name')
	),
	qq(t),
	qq(Confirm that reset timestamp for '$sub2_name' is newer after second reset.)
);

# Get subscription 1 oid
my $sub1_oid = $node_subscriber->safe_psql($db,
	qq(SELECT oid FROM pg_subscription WHERE subname = '$sub1_name'));

# Drop subscription 1
$node_subscriber->safe_psql($db, qq(DROP SUBSCRIPTION $sub1_name));

# Subscription stats for sub1 should be gone
is( $node_subscriber->safe_psql(
		$db, qq(SELECT pg_stat_have_stats('subscription', 0, $sub1_oid))),
	qq(f),
	qq(Subscription stats for subscription '$sub1_name' should be removed.));


$node_subscriber->stop('fast');
$node_publisher->stop('fast');

done_testing();
