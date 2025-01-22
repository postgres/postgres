# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use File::Compare qw(compare_text);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $tempdir = PostgreSQL::Test::Utils::tempdir_short();

# Can be changed to test the other modes.
my $mode = $ENV{PG_TEST_PG_COMBINEBACKUP_MODE} || '--copy';

note "testing using mode $mode";

# Set up a new database instance.
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(has_archiving => 1, allows_streaming => 1);
$primary->append_conf('postgresql.conf', 'summarize_wal = on');
$primary->start;
my $tsprimary = $tempdir . '/ts';
mkdir($tsprimary) || die "mkdir $tsprimary: $!";

# Create some test tables, each containing one row of data, plus a whole
# extra database.
$primary->safe_psql('postgres', <<EOM);
CREATE TABLE will_change (a int, b text);
INSERT INTO will_change VALUES (1, 'initial test row');
CREATE TABLE will_grow (a int, b text);
INSERT INTO will_grow VALUES (1, 'initial test row');
CREATE TABLE will_shrink (a int, b text);
INSERT INTO will_shrink VALUES (1, 'initial test row');
CREATE TABLE will_get_vacuumed (a int, b text);
INSERT INTO will_get_vacuumed VALUES (1, 'initial test row');
CREATE TABLE will_get_dropped (a int, b text);
INSERT INTO will_get_dropped VALUES (1, 'initial test row');
CREATE TABLE will_get_rewritten (a int, b text);
INSERT INTO will_get_rewritten VALUES (1, 'initial test row');
CREATE DATABASE db_will_get_dropped;
CREATE TABLESPACE ts1 LOCATION '$tsprimary';
CREATE TABLE will_not_change_in_ts (a int, b text) TABLESPACE ts1;
INSERT INTO will_not_change_in_ts VALUES (1, 'initial test row');
CREATE TABLE will_change_in_ts (a int, b text) TABLESPACE ts1;
INSERT INTO will_change_in_ts VALUES (1, 'initial test row');
CREATE TABLE will_get_dropped_in_ts (a int, b text);
INSERT INTO will_get_dropped_in_ts VALUES (1, 'initial test row');
EOM

# Read list of tablespace OIDs. There should be just one.
my @tsoids = grep { /^\d+/ } slurp_dir($primary->data_dir . '/pg_tblspc');
is(0 + @tsoids, 1, "exactly one user-defined tablespace");
my $tsoid = $tsoids[0];

# Take a full backup.
my $backup1path = $primary->backup_dir . '/backup1';
my $tsbackup1path = $tempdir . '/ts1backup';
mkdir($tsbackup1path) || die "mkdir $tsbackup1path: $!";
$primary->command_ok(
	[
		'pg_basebackup',
		'--no-sync',
		'--pgdata' => $backup1path,
		'--checkpoint' => 'fast',
		'--tablespace-mapping' => "${tsprimary}=${tsbackup1path}"
	],
	"full backup");

# Now make some database changes.
$primary->safe_psql('postgres', <<EOM);
UPDATE will_change SET b = 'modified value' WHERE a = 1;
UPDATE will_change_in_ts SET b = 'modified value' WHERE a = 1;
INSERT INTO will_grow
	SELECT g, 'additional row' FROM generate_series(2, 5000) g;
TRUNCATE will_shrink;
VACUUM will_get_vacuumed;
DROP TABLE will_get_dropped;
DROP TABLE will_get_dropped_in_ts;
CREATE TABLE newly_created (a int, b text);
INSERT INTO newly_created VALUES (1, 'row for new table');
CREATE TABLE newly_created_in_ts (a int, b text) TABLESPACE ts1;
INSERT INTO newly_created_in_ts VALUES (1, 'row for new table');
VACUUM FULL will_get_rewritten;
DROP DATABASE db_will_get_dropped;
CREATE DATABASE db_newly_created;
EOM

# Take an incremental backup.
my $backup2path = $primary->backup_dir . '/backup2';
my $tsbackup2path = $tempdir . '/tsbackup2';
mkdir($tsbackup2path) || die "mkdir $tsbackup2path: $!";
$primary->command_ok(
	[
		'pg_basebackup',
		'--no-sync',
		'--pgdata' => $backup2path,
		'--checkpoint' => 'fast',
		'--tablespace-mapping' => "${tsprimary}=${tsbackup2path}",
		'--incremental' => $backup1path . '/backup_manifest'
	],
	"incremental backup");

# Find an LSN to which either backup can be recovered.
my $lsn = $primary->safe_psql('postgres', "SELECT pg_current_wal_lsn();");

# Make sure that the WAL segment containing that LSN has been archived.
# PostgreSQL won't issue two consecutive XLOG_SWITCH records, and the backup
# just issued one, so call txid_current() to generate some WAL activity
# before calling pg_switch_wal().
$primary->safe_psql('postgres', 'SELECT txid_current();');
$primary->safe_psql('postgres', 'SELECT pg_switch_wal()');

# Now wait for the LSN we chose above to be archived.
my $archive_wait_query =
  "SELECT pg_walfile_name('$lsn') <= last_archived_wal FROM pg_stat_archiver;";
$primary->poll_query_until('postgres', $archive_wait_query)
  or die "Timed out while waiting for WAL segment to be archived";

# Perform PITR from the full backup. Disable archive_mode so that the archive
# doesn't find out about the new timeline; that way, the later PITR below will
# choose the same timeline.
my $tspitr1path = $tempdir . '/tspitr1';
my $pitr1 = PostgreSQL::Test::Cluster->new('pitr1');
$pitr1->init_from_backup(
	$primary, 'backup1',
	standby => 1,
	has_restoring => 1,
	tablespace_map => { $tsoid => $tspitr1path });
$pitr1->append_conf(
	'postgresql.conf', qq{
recovery_target_lsn = '$lsn'
recovery_target_action = 'promote'
archive_mode = 'off'
});
$pitr1->start();

# Perform PITR to the same LSN from the incremental backup. Use the same
# basic configuration as before.
my $tspitr2path = $tempdir . '/tspitr2';
my $pitr2 = PostgreSQL::Test::Cluster->new('pitr2');
$pitr2->init_from_backup(
	$primary, 'backup2',
	standby => 1,
	has_restoring => 1,
	combine_with_prior => ['backup1'],
	tablespace_map => { $tsbackup2path => $tspitr2path },
	combine_mode => $mode);
$pitr2->append_conf(
	'postgresql.conf', qq{
recovery_target_lsn = '$lsn'
recovery_target_action = 'promote'
archive_mode = 'off'
});
$pitr2->start();

# Wait until both servers exit recovery.
$pitr1->poll_query_until('postgres', "SELECT NOT pg_is_in_recovery();")
  or die "Timed out while waiting apply to reach LSN $lsn";
$pitr2->poll_query_until('postgres', "SELECT NOT pg_is_in_recovery();")
  or die "Timed out while waiting apply to reach LSN $lsn";

# Perform a logical dump of each server, and check that they match.
# It would be much nicer if we could physically compare the data files, but
# that doesn't really work. The contents of the page hole aren't guaranteed to
# be identical, and there can be other discrepancies as well. To make this work
# we'd need the equivalent of each AM's rm_mask function written or at least
# callable from Perl, and that doesn't seem practical.
#
# NB: We're just using the primary's backup directory for scratch space here.
# This could equally well be any other directory we wanted to pick.
my $backupdir = $primary->backup_dir;
my $dump1 = $backupdir . '/pitr1.dump';
my $dump2 = $backupdir . '/pitr2.dump';
$pitr1->command_ok(
	[
		'pg_dumpall',
		'--no-sync',
		'--no-unlogged-table-data',
		'--file' => $dump1,
		'--dbname' => $pitr1->connstr('postgres'),
	],
	'dump from PITR 1');
$pitr2->command_ok(
	[
		'pg_dumpall',
		'--no-sync',
		'--no-unlogged-table-data',
		'--file' => $dump2,
		'--dbname' => $pitr2->connstr('postgres'),
	],
	'dump from PITR 2');

# Compare the two dumps, there should be no differences other than
# the tablespace paths.
my $compare_res = compare_text(
	$dump1, $dump2,
	sub {
		s{create tablespace .* location .*\btspitr\K[12]}{N}i for @_;
		return $_[0] ne $_[1];
	});
note($dump1);
note($dump2);
is($compare_res, 0, "dumps are identical");

# Provide more context if the dumps do not match.
if ($compare_res != 0)
{
	my ($stdout, $stderr) =
	  run_command([ 'diff', '-u', $dump1, $dump2 ]);
	print "=== diff of $dump1 and $dump2\n";
	print "=== stdout ===\n";
	print $stdout;
	print "=== stderr ===\n";
	print $stderr;
	print "=== EOF ===\n";
}

done_testing();
