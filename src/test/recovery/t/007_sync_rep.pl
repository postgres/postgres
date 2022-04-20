
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Minimal test testing synchronous replication sync_state transition
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Query checking sync_priority and sync_state of each standby
my $check_sql =
  "SELECT application_name, sync_priority, sync_state FROM pg_stat_replication ORDER BY application_name;";

# Check that sync_state of each standby is expected (waiting till it is).
# If $setting is given, synchronous_standby_names is set to it and
# the configuration file is reloaded before the test.
sub test_sync_state
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($self, $expected, $msg, $setting) = @_;

	if (defined($setting))
	{
		$self->safe_psql('postgres',
			"ALTER SYSTEM SET synchronous_standby_names = '$setting';");
		$self->reload;
	}

	ok($self->poll_query_until('postgres', $check_sql, $expected), $msg);
	return;
}

# Start a standby and check that it is registered within the WAL sender
# array of the given primary.  This polls the primary's pg_stat_replication
# until the standby is confirmed as registered.
sub start_standby_and_wait
{
	my ($primary, $standby) = @_;
	my $primary_name = $primary->name;
	my $standby_name = $standby->name;
	my $query =
	  "SELECT count(1) = 1 FROM pg_stat_replication WHERE application_name = '$standby_name'";

	$standby->start;

	print("### Waiting for standby \"$standby_name\" on \"$primary_name\"\n");
	$primary->poll_query_until('postgres', $query);
	return;
}

# Initialize primary node
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->start;
my $backup_name = 'primary_backup';

# Take backup
$node_primary->backup($backup_name);

# Create all the standbys.  Their status on the primary is checked to ensure
# the ordering of each one of them in the WAL sender array of the primary.

# Create standby1 linking to primary
my $node_standby_1 = PostgreSQL::Test::Cluster->new('standby1');
$node_standby_1->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
start_standby_and_wait($node_primary, $node_standby_1);

# Create standby2 linking to primary
my $node_standby_2 = PostgreSQL::Test::Cluster->new('standby2');
$node_standby_2->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
start_standby_and_wait($node_primary, $node_standby_2);

# Create standby3 linking to primary
my $node_standby_3 = PostgreSQL::Test::Cluster->new('standby3');
$node_standby_3->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
start_standby_and_wait($node_primary, $node_standby_3);

# Check that sync_state is determined correctly when
# synchronous_standby_names is specified in old syntax.
test_sync_state(
	$node_primary, qq(standby1|1|sync
standby2|2|potential
standby3|0|async),
	'old syntax of synchronous_standby_names',
	'standby1,standby2');

# Check that all the standbys are considered as either sync or
# potential when * is specified in synchronous_standby_names.
# Note that standby1 is chosen as sync standby because
# it's stored in the head of WalSnd array which manages
# all the standbys though they have the same priority.
test_sync_state(
	$node_primary, qq(standby1|1|sync
standby2|1|potential
standby3|1|potential),
	'asterisk in synchronous_standby_names',
	'*');

# Stop and start standbys to rearrange the order of standbys
# in WalSnd array. Now, if standbys have the same priority,
# standby2 is selected preferentially and standby3 is next.
$node_standby_1->stop;
$node_standby_2->stop;
$node_standby_3->stop;

# Make sure that each standby reports back to the primary in the wanted
# order.
start_standby_and_wait($node_primary, $node_standby_2);
start_standby_and_wait($node_primary, $node_standby_3);

# Specify 2 as the number of sync standbys.
# Check that two standbys are in 'sync' state.
test_sync_state(
	$node_primary, qq(standby2|2|sync
standby3|3|sync),
	'2 synchronous standbys',
	'2(standby1,standby2,standby3)');

# Start standby1
start_standby_and_wait($node_primary, $node_standby_1);

# Create standby4 linking to primary
my $node_standby_4 = PostgreSQL::Test::Cluster->new('standby4');
$node_standby_4->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
$node_standby_4->start;

# Check that standby1 and standby2 whose names appear earlier in
# synchronous_standby_names are considered as sync. Also check that
# standby3 appearing later represents potential, and standby4 is
# in 'async' state because it's not in the list.
test_sync_state(
	$node_primary, qq(standby1|1|sync
standby2|2|sync
standby3|3|potential
standby4|0|async),
	'2 sync, 1 potential, and 1 async');

# Check that sync_state of each standby is determined correctly
# when num_sync exceeds the number of names of potential sync standbys
# specified in synchronous_standby_names.
test_sync_state(
	$node_primary, qq(standby1|0|async
standby2|4|sync
standby3|3|sync
standby4|1|sync),
	'num_sync exceeds the num of potential sync standbys',
	'6(standby4,standby0,standby3,standby2)');

# The setting that * comes before another standby name is acceptable
# but does not make sense in most cases. Check that sync_state is
# chosen properly even in case of that setting. standby1 is selected
# as synchronous as it has the highest priority, and is followed by a
# second standby listed first in the WAL sender array, which is
# standby2 in this case.
test_sync_state(
	$node_primary, qq(standby1|1|sync
standby2|2|sync
standby3|2|potential
standby4|2|potential),
	'asterisk before another standby name',
	'2(standby1,*,standby2)');

# Check that the setting of '2(*)' chooses standby2 and standby3 that are stored
# earlier in WalSnd array as sync standbys.
test_sync_state(
	$node_primary, qq(standby1|1|potential
standby2|1|sync
standby3|1|sync
standby4|1|potential),
	'multiple standbys having the same priority are chosen as sync',
	'2(*)');

# Stop Standby3 which is considered in 'sync' state.
$node_standby_3->stop;

# Check that the state of standby1 stored earlier in WalSnd array than
# standby4 is transited from potential to sync.
test_sync_state(
	$node_primary, qq(standby1|1|sync
standby2|1|sync
standby4|1|potential),
	'potential standby found earlier in array is promoted to sync');

# Check that standby1 and standby2 are chosen as sync standbys
# based on their priorities.
test_sync_state(
	$node_primary, qq(standby1|1|sync
standby2|2|sync
standby4|0|async),
	'priority-based sync replication specified by FIRST keyword',
	'FIRST 2(standby1, standby2)');

# Check that all the listed standbys are considered as candidates
# for sync standbys in a quorum-based sync replication.
test_sync_state(
	$node_primary, qq(standby1|1|quorum
standby2|1|quorum
standby4|0|async),
	'2 quorum and 1 async',
	'ANY 2(standby1, standby2)');

# Start Standby3 which will be considered in 'quorum' state.
$node_standby_3->start;

# Check that the setting of 'ANY 2(*)' chooses all standbys as
# candidates for quorum sync standbys.
test_sync_state(
	$node_primary, qq(standby1|1|quorum
standby2|1|quorum
standby3|1|quorum
standby4|1|quorum),
	'all standbys are considered as candidates for quorum sync standbys',
	'ANY 2(*)');

done_testing();
