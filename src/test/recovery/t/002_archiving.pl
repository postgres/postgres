
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# test for archiving with hot standby
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use File::Copy;

# Initialize primary node, doing archives
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(
	has_archiving => 1,
	allows_streaming => 1);
my $backup_name = 'my_backup';

# Start it
$node_primary->start;

# Take backup for standby
$node_primary->backup($backup_name);

# Initialize standby node from backup, fetching WAL from archives
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
# Note that this makes the standby store its contents on the archives
# of the primary.
$node_standby->init_from_backup($node_primary, $backup_name,
	has_restoring => 1);
$node_standby->append_conf('postgresql.conf',
	"wal_retrieve_retry_interval = '100ms'");

# Set archive_cleanup_command and recovery_end_command, checking their
# execution by the backend with dummy commands.
my $data_dir = $node_standby->data_dir;
my $archive_cleanup_command_file = "archive_cleanup_command.done";
my $recovery_end_command_file = "recovery_end_command.done";
$node_standby->append_conf(
	'postgresql.conf', qq(
archive_cleanup_command = 'echo archive_cleanup_done > $archive_cleanup_command_file'
recovery_end_command = 'echo recovery_ended_done > $recovery_end_command_file'
));
$node_standby->start;

# Create some content on primary
$node_primary->safe_psql('postgres',
	"CREATE TABLE tab_int AS SELECT generate_series(1,1000) AS a");

# Note the presence of this checkpoint for the archive_cleanup_command
# check done below, before switching to a new segment.
$node_primary->safe_psql('postgres', "CHECKPOINT");

# Done after the checkpoint to ensure that it is replayed on the standby,
# for archive_cleanup_command.
my $current_lsn =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_lsn();");

# Force archiving of WAL file to make it present on primary
$node_primary->safe_psql('postgres', "SELECT pg_switch_wal()");

# Add some more content, it should not be present on standby
$node_primary->safe_psql('postgres',
	"INSERT INTO tab_int VALUES (generate_series(1001,2000))");

# Wait until necessary replay has been done on standby
my $caughtup_query =
  "SELECT '$current_lsn'::pg_lsn <= pg_last_wal_replay_lsn()";
$node_standby->poll_query_until('postgres', $caughtup_query)
  or die "Timed out while waiting for standby to catch up";

my $result =
  $node_standby->safe_psql('postgres', "SELECT count(*) FROM tab_int");
is($result, qq(1000), 'check content from archives');

# archive_cleanup_command is executed after generating a restart point,
# with a checkpoint.
$node_standby->safe_psql('postgres', q{CHECKPOINT});
ok( -f "$data_dir/$archive_cleanup_command_file",
	'archive_cleanup_command executed on checkpoint');
ok( !-f "$data_dir/$recovery_end_command_file",
	'recovery_end_command not executed yet');

# Check the presence of temporary files specifically generated during
# archive recovery.  To ensure the presence of the temporary history
# file, switch to a timeline large enough to allow a standby to recover
# a history file from an archive.  As this requires at least two timeline
# switches, promote the existing standby first.  Then create a second
# standby based on the primary, using its archives.  Finally, the second
# standby is promoted.
$node_standby->promote;

# Wait until the history file has been stored on the archives of the
# primary once the promotion of the standby completes.  This ensures that
# the second standby created below will be able to restore this file,
# creating a RECOVERYHISTORY.
my $primary_archive = $node_primary->archive_dir;
$caughtup_query =
  "SELECT size IS NOT NULL FROM pg_stat_file('$primary_archive/00000002.history', true)";
$node_primary->poll_query_until('postgres', $caughtup_query)
  or die "Timed out while waiting for archiving of 00000002.history";

# recovery_end_command should have been triggered on promotion.
ok( -f "$data_dir/$recovery_end_command_file",
	'recovery_end_command executed after promotion');

my $node_standby2 = PostgreSQL::Test::Cluster->new('standby2');
$node_standby2->init_from_backup($node_primary, $backup_name,
	has_restoring => 1);

# Make execution of recovery_end_command fail.  This should not affect
# promotion, and its failure should be logged.
$node_standby2->append_conf(
	'postgresql.conf', qq(
recovery_end_command = 'echo recovery_end_failed > missing_dir/xyz.file'
));

$node_standby2->start;

# Save the log location, to see the failure of recovery_end_command.
my $log_location = -s $node_standby2->logfile;

# Now promote standby2, and check that temporary files specifically
# generated during archive recovery are removed by the end of recovery.
$node_standby2->promote;

# Check the logs of the standby to see that the commands have failed.
my $log_contents = slurp_file($node_standby2->logfile, $log_location);
my $node_standby2_data = $node_standby2->data_dir;

like(
	$log_contents,
	qr/restored log file "00000002.history" from archive/s,
	"00000002.history retrieved from the archives");
ok( !-f "$node_standby2_data/pg_wal/RECOVERYHISTORY",
	"RECOVERYHISTORY removed after promotion");
ok( !-f "$node_standby2_data/pg_wal/RECOVERYXLOG",
	"RECOVERYXLOG removed after promotion");
like(
	$log_contents,
	qr/WARNING:.*recovery_end_command/s,
	"recovery_end_command failure detected in logs after promotion");

done_testing();
