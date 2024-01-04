
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Test pg_verifybackup's WAL verification.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Start up the server and take a backup.
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->start;
my $backup_path = $primary->backup_dir . '/test_wal';
$primary->command_ok(
	[ 'pg_basebackup', '-D', $backup_path, '--no-sync', '-cfast' ],
	"base backup ok");

# Rename pg_wal.
my $original_pg_wal = $backup_path . '/pg_wal';
my $relocated_pg_wal = $primary->backup_dir . '/relocated_pg_wal';
rename($original_pg_wal, $relocated_pg_wal) || die "rename pg_wal: $!";

# WAL verification should fail.
command_fails_like(
	[ 'pg_verifybackup', $backup_path ],
	qr/WAL parsing failed for timeline 1/,
	'missing pg_wal causes failure');

# Should work if we skip WAL verification.
command_ok(
	[ 'pg_verifybackup', '-n', $backup_path ],
	'missing pg_wal OK if not verifying WAL');

# Should also work if we specify the correct WAL location.
command_ok([ 'pg_verifybackup', '-w', $relocated_pg_wal, $backup_path ],
	'-w can be used to specify WAL directory');

# Move directory back to original location.
rename($relocated_pg_wal, $original_pg_wal) || die "rename pg_wal back: $!";

# Get a list of files in that directory that look like WAL files.
my @walfiles = grep { /^[0-9A-F]{24}$/ } slurp_dir($original_pg_wal);

# Replace the contents of one of the files with garbage of equal length.
my $wal_corruption_target = $original_pg_wal . '/' . $walfiles[0];
my $wal_size = -s $wal_corruption_target;
open(my $fh, '>', $wal_corruption_target)
  || die "open $wal_corruption_target: $!";
print $fh 'w' x $wal_size;
close($fh);

# WAL verification should fail.
command_fails_like(
	[ 'pg_verifybackup', $backup_path ],
	qr/WAL parsing failed for timeline 1/,
	'corrupt WAL file causes failure');

# Check that WAL-Ranges has correct values with a history file and
# a timeline > 1.  Rather than plugging in a new standby, do a
# self-promotion of this node.
$primary->stop;
$primary->append_conf('standby.signal', '');
$primary->start;
$primary->promote;
$primary->safe_psql('postgres', 'SELECT pg_switch_wal()');
my $backup_path2 = $primary->backup_dir . '/test_tli';
# The base backup run below does a checkpoint, that removes the first segment
# of the current timeline.
$primary->command_ok(
	[ 'pg_basebackup', '-D', $backup_path2, '--no-sync', '-cfast' ],
	"base backup 2 ok");
command_ok(
	[ 'pg_verifybackup', $backup_path2 ],
	'valid base backup with timeline > 1');

done_testing();
