# Copyright (c) 2021-2024, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# For testing purposes, we just want basebackup_to_shell to write standard
# input to a file.  However, Windows doesn't have "cat" or any equivalent, so
# we use "gzip" for this purpose.
my $gzip = $ENV{'GZIP_PROGRAM'};
if (!defined $gzip || $gzip eq '')
{
	plan skip_all => 'gzip not available';
}

# to ensure path can be embedded in postgresql.conf
$gzip =~ s{\\}{/}g if ($PostgreSQL::Test::Utils::windows_os);

my $node = PostgreSQL::Test::Cluster->new('primary');

# Make sure pg_hba.conf is set up to allow connections from backupuser.
# This is only needed on Windows machines that don't use UNIX sockets.
$node->init(
	'allows_streaming' => 1,
	'auth_extra' => [ '--create-role', 'backupuser' ]);

$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'basebackup_to_shell'");
$node->start;
$node->safe_psql('postgres', 'CREATE USER backupuser REPLICATION');
$node->safe_psql('postgres', 'CREATE ROLE trustworthy');

# For nearly all pg_basebackup invocations some options should be specified,
# to keep test times reasonable. Using @pg_basebackup_defs as the first
# element of the array passed to IPC::Run interpolate the array (as it is
# not a reference to an array)...
my @pg_basebackup_defs = ('pg_basebackup', '--no-sync', '-cfast');

# This particular test module generally wants to run with -Xfetch, because
# -Xstream is not supported with a backup target, and with -U backupuser.
my @pg_basebackup_cmd = (@pg_basebackup_defs, '-U', 'backupuser', '-Xfetch');

# Can't use this module without setting basebackup_to_shell.command.
$node->command_fails_like(
	[ @pg_basebackup_cmd, '--target', 'shell' ],
	qr/shell command for backup is not configured/,
	'fails if basebackup_to_shell.command is not set');

# Configure basebackup_to_shell.command and reload the configuration file.
my $backup_path = PostgreSQL::Test::Utils::tempdir;
my $escaped_backup_path = $backup_path;
$escaped_backup_path =~ s{\\}{\\\\}g
  if ($PostgreSQL::Test::Utils::windows_os);
my $shell_command =
  $PostgreSQL::Test::Utils::windows_os
  ? qq{"$gzip" --fast > "$escaped_backup_path\\\\%f.gz"}
  : qq{"$gzip" --fast > "$escaped_backup_path/%f.gz"};
$node->append_conf('postgresql.conf',
	"basebackup_to_shell.command='$shell_command'");
$node->reload();

# Should work now.
$node->command_ok(
	[ @pg_basebackup_cmd, '--target', 'shell' ],
	'backup with no detail: pg_basebackup');
verify_backup('', $backup_path, "backup with no detail");

# Should fail with a detail.
$node->command_fails_like(
	[ @pg_basebackup_cmd, '--target', 'shell:foo' ],
	qr/a target detail is not permitted because the configured command does not include %d/,
	'fails if detail provided without %d');

# Reconfigure to restrict access and require a detail.
$shell_command =
  $PostgreSQL::Test::Utils::windows_os
  ? qq{"$gzip" --fast > "$escaped_backup_path\\\\%d.%f.gz"}
  : qq{"$gzip" --fast > "$escaped_backup_path/%d.%f.gz"};
$node->append_conf('postgresql.conf',
	"basebackup_to_shell.command='$shell_command'");
$node->append_conf('postgresql.conf',
	"basebackup_to_shell.required_role='trustworthy'");
$node->reload();

# Should fail due to lack of permission.
$node->command_fails_like(
	[ @pg_basebackup_cmd, '--target', 'shell' ],
	qr/permission denied to use basebackup_to_shell/,
	'fails if required_role not granted');

# Should fail due to lack of a detail.
$node->safe_psql('postgres', 'GRANT trustworthy TO backupuser');
$node->command_fails_like(
	[ @pg_basebackup_cmd, '--target', 'shell' ],
	qr/a target detail is required because the configured command includes %d/,
	'fails if %d is present and detail not given');

# Should work.
$node->command_ok([ @pg_basebackup_cmd, '--target', 'shell:bar' ],
	'backup with detail: pg_basebackup');
verify_backup('bar.', $backup_path, "backup with detail");

done_testing();

sub verify_backup
{
	my ($prefix, $backup_dir, $test_name) = @_;

	ok( -f "$backup_dir/${prefix}backup_manifest.gz",
		"$test_name: backup_manifest.gz was created");
	ok( -f "$backup_dir/${prefix}base.tar.gz",
		"$test_name: base.tar.gz was created");

  SKIP:
	{
		my $tar = $ENV{TAR};
		skip "no tar program available", 1 if (!defined $tar || $tar eq '');

		# Decompress.
		system_or_bail($gzip, '-d',
			$backup_dir . '/' . $prefix . 'backup_manifest.gz');
		system_or_bail($gzip, '-d',
			$backup_dir . '/' . $prefix . 'base.tar.gz');

		# Untar.
		my $extract_path = PostgreSQL::Test::Utils::tempdir;
		system_or_bail($tar, 'xf', $backup_dir . '/' . $prefix . 'base.tar',
			'-C', $extract_path);

		# Verify.
		$node->command_ok(
			[
				'pg_verifybackup', '-n',
				'-m', "${backup_dir}/${prefix}backup_manifest",
				'-e', $extract_path
			],
			"$test_name: backup verifies ok");
	}
}
