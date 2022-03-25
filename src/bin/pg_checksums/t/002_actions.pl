
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Do basic sanity checks supported by pg_checksums using
# an initialized cluster.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;

use Test::More;


# Utility routine to create and check a table with corrupted checksums
# on a wanted tablespace.  Note that this stops and starts the node
# multiple times to perform the checks, leaving the node started
# at the end.
sub check_relation_corruption
{
	my $node       = shift;
	my $table      = shift;
	my $tablespace = shift;
	my $pgdata     = $node->data_dir;

	# Create table and discover its filesystem location.
	$node->safe_psql(
		'postgres',
		"CREATE TABLE $table AS SELECT a FROM generate_series(1,10000) AS a;
		ALTER TABLE $table SET (autovacuum_enabled=false);");

	$node->safe_psql('postgres',
		"ALTER TABLE " . $table . " SET TABLESPACE " . $tablespace . ";");

	my $file_corrupted =
	  $node->safe_psql('postgres', "SELECT pg_relation_filepath('$table');");
	my $relfilenode_corrupted = $node->safe_psql('postgres',
		"SELECT relfilenode FROM pg_class WHERE relname = '$table';");

	$node->stop;

	# Checksums are correct for single relfilenode as the table is not
	# corrupted yet.
	command_ok(
		[
			'pg_checksums', '--check',
			'-D',           $pgdata,
			'--filenode',   $relfilenode_corrupted
		],
		"succeeds for single relfilenode on tablespace $tablespace with offline cluster"
	);

	# Time to create some corruption
	$node->corrupt_page_checksum($file_corrupted, 0);

	# Checksum checks on single relfilenode fail
	$node->command_checks_all(
		[
			'pg_checksums', '--check',
			'-D',           $pgdata,
			'--filenode',   $relfilenode_corrupted
		],
		1,
		[qr/Bad checksums:.*1/],
		[qr/checksum verification failed/],
		"fails with corrupted data for single relfilenode on tablespace $tablespace"
	);

	# Global checksum checks fail as well
	$node->command_checks_all(
		[ 'pg_checksums', '--check', '-D', $pgdata ],
		1,
		[qr/Bad checksums:.*1/],
		[qr/checksum verification failed/],
		"fails with corrupted data on tablespace $tablespace");

	# Drop corrupted table again and make sure there is no more corruption.
	$node->start;
	$node->safe_psql('postgres', "DROP TABLE $table;");
	$node->stop;
	$node->command_ok([ 'pg_checksums', '--check', '-D', $pgdata ],
		"succeeds again after table drop on tablespace $tablespace");

	$node->start;
	return;
}

# Initialize node with checksums disabled.
my $node = PostgreSQL::Test::Cluster->new('node_checksum');
$node->init();
my $pgdata = $node->data_dir;

# Control file should know that checksums are disabled.
command_like(
	[ 'pg_controldata', $pgdata ],
	qr/Data page checksum version:.*0/,
	'checksums disabled in control file');

# These are correct but empty files, so they should pass through.
append_to_file "$pgdata/global/99999",          "";
append_to_file "$pgdata/global/99999.123",      "";
append_to_file "$pgdata/global/99999_fsm",      "";
append_to_file "$pgdata/global/99999_init",     "";
append_to_file "$pgdata/global/99999_vm",       "";
append_to_file "$pgdata/global/99999_init.123", "";
append_to_file "$pgdata/global/99999_fsm.123",  "";
append_to_file "$pgdata/global/99999_vm.123",   "";

# These are temporary files and folders with dummy contents, which
# should be ignored by the scan.
append_to_file "$pgdata/global/pgsql_tmp_123", "foo";
mkdir "$pgdata/global/pgsql_tmp";
append_to_file "$pgdata/global/pgsql_tmp/1.1",        "foo";
append_to_file "$pgdata/global/pg_internal.init",     "foo";
append_to_file "$pgdata/global/pg_internal.init.123", "foo";

# Enable checksums.
command_ok([ 'pg_checksums', '--enable', '--no-sync', '-D', $pgdata ],
	"checksums successfully enabled in cluster");

# Successive attempt to enable checksums fails.
command_fails([ 'pg_checksums', '--enable', '--no-sync', '-D', $pgdata ],
	"enabling checksums fails if already enabled");

# Control file should know that checksums are enabled.
command_like(
	[ 'pg_controldata', $pgdata ],
	qr/Data page checksum version:.*1/,
	'checksums enabled in control file');

# Disable checksums again.  Flush result here as that should be cheap.
command_ok(
	[ 'pg_checksums', '--disable', '-D', $pgdata ],
	"checksums successfully disabled in cluster");

# Successive attempt to disable checksums fails.
command_fails(
	[ 'pg_checksums', '--disable', '--no-sync', '-D', $pgdata ],
	"disabling checksums fails if already disabled");

# Control file should know that checksums are disabled.
command_like(
	[ 'pg_controldata', $pgdata ],
	qr/Data page checksum version:.*0/,
	'checksums disabled in control file');

# Enable checksums again for follow-up tests.
command_ok([ 'pg_checksums', '--enable', '--no-sync', '-D', $pgdata ],
	"checksums successfully enabled in cluster");

# Control file should know that checksums are enabled.
command_like(
	[ 'pg_controldata', $pgdata ],
	qr/Data page checksum version:.*1/,
	'checksums enabled in control file');

# Checksums pass on a newly-created cluster
command_ok([ 'pg_checksums', '--check', '-D', $pgdata ],
	"succeeds with offline cluster");

# Checksums are verified if no other arguments are specified
command_ok(
	[ 'pg_checksums', '-D', $pgdata ],
	"verifies checksums as default action");

# Specific relation files cannot be requested when action is --disable
# or --enable.
command_fails(
	[ 'pg_checksums', '--disable', '--filenode', '1234', '-D', $pgdata ],
	"fails when relfilenodes are requested and action is --disable");
command_fails(
	[ 'pg_checksums', '--enable', '--filenode', '1234', '-D', $pgdata ],
	"fails when relfilenodes are requested and action is --enable");

# Test postgres -C for an offline cluster.
# Run-time GUCs are safe to query here.  Note that a lock file is created,
# then removed, leading to an extra LOG entry showing in stderr.  This uses
# log_min_messages=fatal to remove any noise.  This test uses a startup
# wrapped with pg_ctl to allow the case where this runs under a privileged
# account on Windows.
command_checks_all(
	[
		'pg_ctl', 'start', '-D', $pgdata, '-s', '-o',
		'-C data_checksums -c log_min_messages=fatal'
	],
	1,
	[qr/^on$/],
	[qr/could not start server/],
	'data_checksums=on is reported on an offline cluster');

# Checks cannot happen with an online cluster
$node->start;
command_fails([ 'pg_checksums', '--check', '-D', $pgdata ],
	"fails with online cluster");

# Check corruption of table on default tablespace.
check_relation_corruption($node, 'corrupt1', 'pg_default');

# Create tablespace to check corruptions in a non-default tablespace.
my $basedir        = $node->basedir;
my $tablespace_dir = "$basedir/ts_corrupt_dir";
mkdir($tablespace_dir);
$node->safe_psql('postgres',
	"CREATE TABLESPACE ts_corrupt LOCATION '$tablespace_dir';");
check_relation_corruption($node, 'corrupt2', 'ts_corrupt');

# Utility routine to check that pg_checksums is able to detect
# correctly-named relation files filled with some corrupted data.
sub fail_corrupt
{
	my $node   = shift;
	my $file   = shift;
	my $pgdata = $node->data_dir;

	# Create the file with some dummy data in it.
	my $file_name = "$pgdata/global/$file";
	append_to_file $file_name, "foo";

	$node->command_checks_all(
		[ 'pg_checksums', '--check', '-D', $pgdata ],
		1,
		[qr/^$/],
		[qr/could not read block 0 in file.*$file\":/],
		"fails for corrupted data in $file");

	# Remove file to prevent future lookup errors on conflicts.
	unlink $file_name;
	return;
}

# Stop instance for the follow-up checks.
$node->stop;

# Create a fake tablespace location that should not be scanned
# when verifying checksums.
mkdir "$tablespace_dir/PG_99_999999991/";
append_to_file "$tablespace_dir/PG_99_999999991/foo", "123";
command_ok([ 'pg_checksums', '--check', '-D', $pgdata ],
	"succeeds with foreign tablespace");

# Authorized relation files filled with corrupted data cause the
# checksum checks to fail.  Make sure to use file names different
# than the previous ones.
fail_corrupt($node, "99990");
fail_corrupt($node, "99990.123");
fail_corrupt($node, "99990_fsm");
fail_corrupt($node, "99990_init");
fail_corrupt($node, "99990_vm");
fail_corrupt($node, "99990_init.123");
fail_corrupt($node, "99990_fsm.123");
fail_corrupt($node, "99990_vm.123");

done_testing();
