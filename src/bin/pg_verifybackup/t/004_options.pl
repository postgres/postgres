# Verify the behavior of assorted pg_verifybackup options.

use strict;
use warnings;
use Cwd;
use Config;
use File::Path qw(rmtree);
use PostgresNode;
use TestLib;
use Test::More tests => 25;

# Start up the server and take a backup.
my $master = get_new_node('master');
$master->init(allows_streaming => 1);
$master->start;
my $backup_path = $master->backup_dir . '/test_options';
$master->command_ok([ 'pg_basebackup', '-D', $backup_path, '--no-sync' ],
	"base backup ok");

# Verify that pg_verifybackup -q succeeds and produces no output.
my $stdout;
my $stderr;
my $result = IPC::Run::run [ 'pg_verifybackup', '-q', $backup_path ],
  '>', \$stdout, '2>', \$stderr;
ok($result, "-q succeeds: exit code 0");
is($stdout, '', "-q succeeds: no stdout");
is($stderr, '', "-q succeeds: no stderr");

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

# Validation should succeed if we ignore the problem file.
command_like(
	[ 'pg_verifybackup', '-i', 'PG_VERSION', $backup_path ],
	qr/backup successfully verified/,
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
		'pg_verifybackup',              '-m',
		"$backup_path/backup_manifest", "$backup_path/fake"
	],
	qr/could not open directory/,
	'nonexistent backup directory');
