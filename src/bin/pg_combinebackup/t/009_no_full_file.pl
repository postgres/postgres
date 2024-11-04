# Copyright (c) 2021-2024, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use File::Copy;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Can be changed to test the other modes.
my $mode = $ENV{PG_TEST_PG_COMBINEBACKUP_MODE} || '--copy';

note "testing using mode $mode";

# Set up a new database instance.
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(has_archiving => 1, allows_streaming => 1);
$primary->append_conf('postgresql.conf', 'summarize_wal = on');
$primary->start;

# Take a full backup.
my $backup1path = $primary->backup_dir . '/backup1';
$primary->command_ok(
	[ 'pg_basebackup', '-D', $backup1path, '--no-sync', '-cfast' ],
	"full backup");

# Take an incremental backup.
my $backup2path = $primary->backup_dir . '/backup2';
$primary->command_ok(
	[
		'pg_basebackup', '-D', $backup2path, '--no-sync', '-cfast',
		'--incremental', $backup1path . '/backup_manifest'
	],
	"incremental backup");

# Find an incremental file in the incremental backup for which there is a full
# file in the full backup. When we find one, replace the full file with an
# incremental file.
my @filelist = grep { /^INCREMENTAL\./ } slurp_dir("$backup2path/base/1");
my $success = 0;
for my $iname (@filelist)
{
	my $name = $iname;
	$name =~ s/^INCREMENTAL.//;

	if (-f "$backup1path/base/1/$name")
	{
		copy("$backup2path/base/1/$iname", "$backup1path/base/1/$iname")
			|| die "copy $backup2path/base/1/$iname: $!";
		unlink("$backup1path/base/1/$name")
			|| die "unlink $backup1path/base/1/$name: $!";
		$success = 1;
		last;
	}
}

# pg_combinebackup should fail.
my $outpath = $primary->backup_dir . '/out';
$primary->command_fails_like(
	[
		'pg_combinebackup', $backup1path, $backup2path, '-o', $outpath,
	],
	qr/full backup contains unexpected incremental file/,
	"pg_combinebackup fails");

done_testing();
