
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# Checks for recovery_min_apply_delay and recovery pause
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize primary node
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->start;

# And some content
$node_primary->safe_psql('postgres',
	"CREATE TABLE tab_int AS SELECT generate_series(1, 10) AS a");

# Take backup
my $backup_name = 'my_backup';
$node_primary->backup($backup_name);

# Create streaming standby from backup
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
my $delay = 3;
$node_standby->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
$node_standby->append_conf(
	'postgresql.conf', qq(
recovery_min_apply_delay = '${delay}s'
));
$node_standby->start;

# Make new content on primary and check its presence in standby depending
# on the delay applied above. Before doing the insertion, get the
# current timestamp that will be used as a comparison base. Even on slow
# machines, this allows to have a predictable behavior when comparing the
# delay between data insertion moment on primary and replay time on standby.
my $primary_insert_time = time();
$node_primary->safe_psql('postgres',
	"INSERT INTO tab_int VALUES (generate_series(11, 20))");

# Now wait for replay to complete on standby. We're done waiting when the
# standby has replayed up to the previously saved primary LSN.
my $until_lsn =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_lsn()");

$node_standby->poll_query_until('postgres',
	"SELECT (pg_last_wal_replay_lsn() - '$until_lsn'::pg_lsn) >= 0")
  or die "standby never caught up";

# This test is successful if and only if the LSN has been applied with at least
# the configured apply delay.
ok(time() - $primary_insert_time >= $delay,
	"standby applies WAL only after replication delay");


# Check that recovery can be paused or resumed expectedly.
my $node_standby2 = PostgreSQL::Test::Cluster->new('standby2');
$node_standby2->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
$node_standby2->start;

# Recovery is not yet paused.
is( $node_standby2->safe_psql(
		'postgres', "SELECT pg_get_wal_replay_pause_state()"),
	'not paused',
	'pg_get_wal_replay_pause_state() reports not paused');

# Request to pause recovery and wait until it's actually paused.
$node_standby2->safe_psql('postgres', "SELECT pg_wal_replay_pause()");
$node_primary->safe_psql('postgres',
	"INSERT INTO tab_int VALUES (generate_series(21,30))");
$node_standby2->poll_query_until('postgres',
	"SELECT pg_get_wal_replay_pause_state() = 'paused'")
  or die "Timed out while waiting for recovery to be paused";

# Even if new WAL records are streamed from the primary,
# recovery in the paused state doesn't replay them.
my $receive_lsn =
  $node_standby2->safe_psql('postgres', "SELECT pg_last_wal_receive_lsn()");
my $replay_lsn =
  $node_standby2->safe_psql('postgres', "SELECT pg_last_wal_replay_lsn()");
$node_primary->safe_psql('postgres',
	"INSERT INTO tab_int VALUES (generate_series(31,40))");
$node_standby2->poll_query_until('postgres',
	"SELECT '$receive_lsn'::pg_lsn < pg_last_wal_receive_lsn()")
  or die "Timed out while waiting for new WAL to be streamed";
is( $node_standby2->safe_psql('postgres', "SELECT pg_last_wal_replay_lsn()"),
	qq($replay_lsn),
	'no WAL is replayed in the paused state');

# Request to resume recovery and wait until it's actually resumed.
$node_standby2->safe_psql('postgres', "SELECT pg_wal_replay_resume()");
$node_standby2->poll_query_until('postgres',
	"SELECT pg_get_wal_replay_pause_state() = 'not paused' AND pg_last_wal_replay_lsn() > '$replay_lsn'::pg_lsn"
) or die "Timed out while waiting for recovery to be resumed";

# Check that the paused state ends and promotion continues if a promotion
# is triggered while recovery is paused.
$node_standby2->safe_psql('postgres', "SELECT pg_wal_replay_pause()");
$node_primary->safe_psql('postgres',
	"INSERT INTO tab_int VALUES (generate_series(41,50))");
$node_standby2->poll_query_until('postgres',
	"SELECT pg_get_wal_replay_pause_state() = 'paused'")
  or die "Timed out while waiting for recovery to be paused";

$node_standby2->promote;
$node_standby2->poll_query_until('postgres', "SELECT NOT pg_is_in_recovery()")
  or die "Timed out while waiting for promotion to finish";

done_testing();
