
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

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
	[ 'pg_basebackup', '-D', $backup_path, '--no-sync', '-cfast' ],
	"base backup ok");

# Verify that pg_verifybackup -q succeeds and produces no output.
my $stdout;
my $stderr;
my $result = IPC::Run::run [ 'pg_verifybackup', '-q', $backup_path ],
  '>', \$stdout, '2>', \$stderr;
ok($result, "-q succeeds: exit code 0");
is($stdout, '', "-q succeeds: no stdout");
is($stderr, '', "-q succeeds: no stderr");

# Should still work if we specify -Fp.
$primary->command_ok(
	[ 'pg_verifybackup', '-Fp', $backup_path ],
	"verifies with -Fp");

# Should not work if we specify -Fy because that's invalid.
$primary->command_fails_like(
	[ 'pg_verifybackup', '-Fy', $backup_path ],
	qr(invalid backup format "y", must be "plain" or "tar"),
	"does not verify with -Fy");

# Should produce a lengthy list of errors; we test for just one of those.
$primary->command_fails_like(
	[ 'pg_verifybackup', '-Ft', '-n', $backup_path ],
	qr("pg_multixact" is not a plain file),
	"does not verify with -Ft -n");

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
	[ 'pg_verifybackup', '-q', $backup_path ],
	qr/checksum mismatch for file \"PG_VERSION\"/,
	'-q checksum mismatch');

# Since we didn't change the length of the file, verification should succeed
# if we ignore checksums. Check that we get the right message, too.
command_like(
	[ 'pg_verifybackup', '-s', $backup_path ],
	qr/backup successfully verified/,
	'-s skips checksumming');

# Validation should succeed if we ignore the problem file. Also, check
# the progress information.
command_checks_all(
	[ 'pg_verifybackup', '--progress', '-i', 'PG_VERSION', $backup_path ],
	0,
	[qr/backup successfully verified/],
	[qr{(\d+/\d+ kB \(\d+%\) verified)+}],
	'-i ignores problem file');

# PG_VERSION is already corrupt; let's try also removing all of pg_xact.
rmtree($backup_path . "/pg_xact");

# We're ignoring the problem with PG_VERSION, but not the problem with
# pg_xact, so verification should fail here.
command_fails_like(
	[ 'pg_verifybackup', '-i', 'PG_VERSION', $backup_path ],
	qr/pg_xact.*is present in the manifest but not on disk/,
	'-i does not ignore all problems');

# If we use -i twice, we should be able to ignore all of the problems.
command_like(
	[ 'pg_verifybackup', '-i', 'PG_VERSION', '-i', 'pg_xact', $backup_path ],
	qr/backup successfully verified/,
	'multiple -i options work');

# Verify that when -i is not used, both problems are reported.
$result = IPC::Run::run [ 'pg_verifybackup', $backup_path ],
  '>', \$stdout, '2>', \$stderr;
ok(!$result, "multiple problems: fails");
like(
	$stderr,
	qr/pg_xact.*is present in the manifest but not on disk/,
	"multiple problems: missing files reported");
like(
	$stderr,
	qr/checksum mismatch for file \"PG_VERSION\"/,
	"multiple problems: checksum mismatch reported");

# Verify that when -e is used, only the problem detected first is reported.
$result = IPC::Run::run [ 'pg_verifybackup', '-e', $backup_path ],
  '>', \$stdout, '2>', \$stderr;
ok(!$result, "-e reports 1 error: fails");
like(
	$stderr,
	qr/pg_xact.*is present in the manifest but not on disk/,
	"-e reports 1 error: missing files reported");
unlike(
	$stderr,
	qr/checksum mismatch for file \"PG_VERSION\"/,
	"-e reports 1 error: checksum mismatch not reported");

# Test valid manifest with nonexistent backup directory.
command_fails_like(
	[
		'pg_verifybackup', '-m',
		"$backup_path/backup_manifest", "$backup_path/fake"
	],
	qr/could not open directory/,
	'nonexistent backup directory');

done_testing();
