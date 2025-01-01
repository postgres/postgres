
# Copyright (c) 2024-2025, PostgreSQL Global Development Group

# Test low-level backup method by using pg_backup_start() and pg_backup_stop()
# to create backups.

use strict;
use warnings FATAL => 'all';

use File::Copy qw(copy);
use File::Path qw(rmtree);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Start primary node with archiving.
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(has_archiving => 1, allows_streaming => 1);
$node_primary->start;

# Start backup.
my $backup_name = 'backup1';
my $psql = $node_primary->background_psql('postgres');

$psql->query_safe("SET client_min_messages TO WARNING");
$psql->set_query_timer_restart;
$psql->query_safe("select pg_backup_start('test label')");

# Copy files.
my $backup_dir = $node_primary->backup_dir . '/' . $backup_name;

PostgreSQL::Test::RecursiveCopy::copypath($node_primary->data_dir,
	$backup_dir);

# Cleanup some files/paths that should not be in the backup.  There is no
# attempt to handle all the exclusions done by pg_basebackup here, in part
# because these are not required, but also to keep the test simple.
#
# Also remove pg_control because it needs to be copied later.
unlink("$backup_dir/postmaster.pid")
  or BAIL_OUT("unable to unlink $backup_dir/postmaster.pid");
unlink("$backup_dir/postmaster.opts")
  or BAIL_OUT("unable to unlink $backup_dir/postmaster.opts");
unlink("$backup_dir/global/pg_control")
  or BAIL_OUT("unable to unlink $backup_dir/global/pg_control");

rmtree("$backup_dir/pg_wal")
  or BAIL_OUT("unable to unlink contents of $backup_dir/pg_wal");
mkdir("$backup_dir/pg_wal");

# Create a table that will be used to verify that recovery started at the
# correct location, rather than a location recorded in the control file.
$node_primary->safe_psql('postgres', "create table canary (id int)");

# Advance the checkpoint location in pg_control past the location where the
# backup started.  Switch WAL to make it really clear that the location is
# different and to put the checkpoint in a new WAL segment.
my $segment_name = $node_primary->safe_psql('postgres',
	"select pg_walfile_name(pg_switch_wal())");

# Ensure that the segment just switched from is archived.  The follow-up
# tests depend on its presence to begin recovery.
$node_primary->poll_query_until('postgres',
	q{SELECT last_archived_wal FROM pg_stat_archiver},
	$segment_name)
  or die
  "Timed out while waiting for archiving of switched segment to finish";

$node_primary->safe_psql('postgres', "checkpoint");

# Copy pg_control last so it contains the new checkpoint.
copy($node_primary->data_dir . '/global/pg_control',
	"$backup_dir/global/pg_control")
  or BAIL_OUT("unable to copy global/pg_control");

# Save the name segment that will be archived by pg_backup_stop().
# This is copied to the pg_wal directory of the node whose recovery
# is done without a backup_label.
my $stop_segment_name = $node_primary->safe_psql('postgres',
	'SELECT pg_walfile_name(pg_current_wal_lsn())');

# Stop backup and get backup_label, the last segment is archived.
my $backup_label =
  $psql->query_safe("select labelfile from pg_backup_stop()");

$psql->quit;

# Rather than writing out backup_label, try to recover the backup without
# backup_label to demonstrate that recovery will not work correctly without it,
# i.e. the canary table will be missing and the cluster will be corrupted.
# Provide only the WAL segment that recovery will think it needs.
#
# The point of this test is to explicitly demonstrate that backup_label is
# being used in a later test to get the correct recovery info.
my $node_replica = PostgreSQL::Test::Cluster->new('replica_fail');
$node_replica->init_from_backup($node_primary, $backup_name);
$node_replica->append_conf('postgresql.conf', "archive_mode = off");

my $canary_query = "select count(*) from pg_class where relname = 'canary'";

copy(
	$node_primary->archive_dir . "/$stop_segment_name",
	$node_replica->data_dir . "/pg_wal/$stop_segment_name"
) or BAIL_OUT("unable to copy $stop_segment_name");

$node_replica->start;

ok($node_replica->safe_psql('postgres', $canary_query) == 0,
	'canary is missing');

# Check log to ensure that crash recovery was used as there is no
# backup_label.
ok( $node_replica->log_contains(
		'database system was not properly shut down; automatic recovery in progress'
	),
	'verify backup recovery performed with crash recovery');

$node_replica->teardown_node;
$node_replica->clean_node;

# Save backup_label into the backup directory and recover using the primary's
# archive.  This time recovery will succeed and the canary table will be
# present.
open my $fh, ">>", "$backup_dir/backup_label"
  or die "could not open backup_label";
# Binary mode is required for Windows, as the backup_label parsing is not
# able to cope with CRLFs.
binmode $fh;
print $fh $backup_label;
close $fh;

$node_replica = PostgreSQL::Test::Cluster->new('replica_success');
$node_replica->init_from_backup($node_primary, $backup_name,
	has_restoring => 1);
$node_replica->start;

ok($node_replica->safe_psql('postgres', $canary_query) == 1,
	'canary is present');

# Check log to ensure that backup_label was used for recovery.
ok($node_replica->log_contains('starting backup recovery with redo LSN'),
	'verify backup recovery performed with backup_label');

done_testing();
