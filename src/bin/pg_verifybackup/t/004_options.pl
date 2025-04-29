
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# Verify the behavior of assorted pg_verifybackup options.

use strict;
use warnings FATAL => 'all';
use File::Path qw(rmtree);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Start up the server and take a backup.
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->start;
my $backup_path = $primary->backup_dir . '/test_options';
$primary->command_ok(
	[
		'pg_basebackup',
		'--pgdata' => $backup_path,
		'--no-sync',
		'--checkpoint' => 'fast'
	],
	"base backup ok");

# Verify that pg_verifybackup --quiet succeeds and produces no output.
my $stdout;
my $stderr;
my $result = IPC::Run::run [ 'pg_verifybackup', '--quiet', $backup_path ],
  '>' => \$stdout,
  '2>' => \$stderr;
ok($result, "--quiet succeeds: exit code 0");
is($stdout, '', "--quiet succeeds: no stdout");
is($stderr, '', "--quiet succeeds: no stderr");

# Should still work if we specify --format=plain.
$primary->command_ok(
	[ 'pg_verifybackup', '--format' => 'plain', $backup_path ],
	"verifies with --format=plain");

# Should not work if we specify --format=y because that's invalid.
$primary->command_fails_like(
	[ 'pg_verifybackup', '--format' => 'y', $backup_path ],
	qr(invalid backup format "y", must be "plain" or "tar"),
	"does not verify with --format=y");

# Should produce a lengthy list of errors; we test for just one of those.
$primary->command_fails_like(
	[
		'pg_verifybackup',
		'--format' => 'tar',
		'--no-parse-wal',
		$backup_path
	],
	qr("pg_multixact" is not a regular file),
	"does not verify with --format=tar --no-parse-wal");

# Test invalid options
command_fails_like(
	[ 'pg_verifybackup', '--progress', '--quiet', $backup_path ],
	qr{cannot specify both -P/--progress and -q/--quiet},
	'cannot use --progress and --quiet at the same time');

# Corrupt the PG_VERSION file.
my $version_pathname = "$backup_path/PG_VERSION";
my $version_contents = slurp_file($version_pathname);
open(my $fh, '>', $version_pathname) || die "open $version_pathname: $!";
print $fh 'q' x length($version_contents);
close($fh);

# Verify that pg_verifybackup -q now fails.
command_fails_like(
	[ 'pg_verifybackup', '--quiet', $backup_path ],
	qr/checksum mismatch for file \"PG_VERSION\"/,
	'--quiet checksum mismatch');

# Since we didn't change the length of the file, verification should succeed
# if we ignore checksums. Check that we get the right message, too.
command_like(
	[ 'pg_verifybackup', '--skip-checksums', $backup_path ],
	qr/backup successfully verified/,
	'--skip-checksums skips checksumming');

# Validation should succeed if we ignore the problem file. Also, check
# the progress information.
command_checks_all(
	[
		'pg_verifybackup',
		'--progress',
		'--ignore' => 'PG_VERSION',
		$backup_path
	],
	0,
	[qr/backup successfully verified/],
	[qr{(\d+/\d+ kB \(\d+%\) verified)+}],
	'--ignore ignores problem file');

# PG_VERSION is already corrupt; let's try also removing all of pg_xact.
rmtree($backup_path . "/pg_xact");

# We're ignoring the problem with PG_VERSION, but not the problem with
# pg_xact, so verification should fail here.
command_fails_like(
	[ 'pg_verifybackup', '--ignore' => 'PG_VERSION', $backup_path ],
	qr/pg_xact.*is present in the manifest but not on disk/,
	'--ignore does not ignore all problems');

# If we use --ignore twice, we should be able to ignore all of the problems.
command_like(
	[
		'pg_verifybackup',
		'--ignore' => 'PG_VERSION',
		'--ignore' => 'pg_xact',
		$backup_path
	],
	qr/backup successfully verified/,
	'multiple --ignore options work');

# Verify that when --ignore is not used, both problems are reported.
$result = IPC::Run::run [ 'pg_verifybackup', $backup_path ],
  '>' => \$stdout,
  '2>' => \$stderr;
ok(!$result, "multiple problems: fails");
like(
	$stderr,
	qr/pg_xact.*is present in the manifest but not on disk/,
	"multiple problems: missing files reported");
like(
	$stderr,
	qr/checksum mismatch for file \"PG_VERSION\"/,
	"multiple problems: checksum mismatch reported");

# Verify that when --exit-on-error is used, only the problem detected
# first is reported.
$result =
  IPC::Run::run [ 'pg_verifybackup', '--exit-on-error', $backup_path ],
  '>' => \$stdout,
  '2>' => \$stderr;
ok(!$result, "--exit-on-error reports 1 error: fails");
like(
	$stderr,
	qr/pg_xact.*is present in the manifest but not on disk/,
	"--exit-on-error reports 1 error: missing files reported");
unlike(
	$stderr,
	qr/checksum mismatch for file \"PG_VERSION\"/,
	"--exit-on-error reports 1 error: checksum mismatch not reported");

# Test valid manifest with nonexistent backup directory.
command_fails_like(
	[
		'pg_verifybackup',
		'--manifest-path' => "$backup_path/backup_manifest",
		"$backup_path/fake"
	],
	qr/could not open directory/,
	'nonexistent backup directory');

done_testing();
