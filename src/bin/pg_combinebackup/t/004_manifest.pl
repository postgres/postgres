# Copyright (c) 2021-2024, PostgreSQL Global Development Group
#
# This test aims to validate that pg_combinebackup works in the degenerate
# case where it is invoked on a single full backup and that it can produce
# a new, valid manifest when it does. Secondarily, it checks that
# pg_combinebackup does not produce a manifest when run with --no-manifest.

use strict;
use warnings FATAL => 'all';
use File::Compare;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Can be changed to test the other modes.
my $mode = $ENV{PG_TEST_PG_COMBINEBACKUP_MODE} || '--copy';

note "testing using mode $mode";

# Set up a new database instance.
my $node = PostgreSQL::Test::Cluster->new('node');
$node->init(has_archiving => 1, allows_streaming => 1);
$node->start;

# Take a full backup.
my $original_backup_path = $node->backup_dir . '/original';
$node->command_ok(
	[ 'pg_basebackup', '-D', $original_backup_path, '--no-sync', '-cfast' ],
	"full backup");

# Verify the full backup.
$node->command_ok([ 'pg_verifybackup', $original_backup_path ],
	"verify original backup");

# Process the backup with pg_combinebackup using various manifest options.
sub combine_and_test_one_backup
{
	my ($backup_name, $failure_pattern, @extra_options) = @_;
	my $revised_backup_path = $node->backup_dir . '/' . $backup_name;
	$node->command_ok(
		[
			'pg_combinebackup', $original_backup_path,
			'-o', $revised_backup_path,
			'--no-sync', @extra_options
		],
		"pg_combinebackup with @extra_options");
	if (defined $failure_pattern)
	{
		$node->command_fails_like([ 'pg_verifybackup', $revised_backup_path ],
			$failure_pattern, "unable to verify backup $backup_name");
	}
	else
	{
		$node->command_ok([ 'pg_verifybackup', $revised_backup_path ],
			"verify backup $backup_name");
	}
}
combine_and_test_one_backup('nomanifest',
	qr/could not open file.*backup_manifest/,
	'--no-manifest');
combine_and_test_one_backup('csum_none', undef, '--manifest-checksums=NONE',
	$mode);
combine_and_test_one_backup('csum_sha224',
	undef, '--manifest-checksums=SHA224', $mode);

# Verify that SHA224 is mentioned in the SHA224 manifest lots of times.
my $sha224_manifest =
  slurp_file($node->backup_dir . '/csum_sha224/backup_manifest');
my $sha224_count = (() = $sha224_manifest =~ /SHA224/mig);
cmp_ok($sha224_count,
	'>', 100, "SHA224 is mentioned many times in SHA224 manifest");

# Verify that SHA224 is mentioned in the SHA224 manifest lots of times.
my $nocsum_manifest =
  slurp_file($node->backup_dir . '/csum_none/backup_manifest');
my $nocsum_count = (() = $nocsum_manifest =~ /Checksum-Algorithm/mig);
is($nocsum_count, 0,
	"Checksum-Algorithm is not mentioned in no-checksum manifest");

# OK, that's all.
done_testing();
