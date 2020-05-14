# Verify that we can take and verify backups with various checksum types.

use strict;
use warnings;
use Cwd;
use Config;
use File::Path qw(rmtree);
use PostgresNode;
use TestLib;
use Test::More tests => 19;

my $master = get_new_node('master');
$master->init(allows_streaming => 1);
$master->start;

for my $algorithm (qw(bogus none crc32c sha224 sha256 sha384 sha512))
{
	my $backup_path = $master->backup_dir . '/' . $algorithm;
	my @backup      = (
		'pg_basebackup', '-D', $backup_path,
		'--manifest-checksums', $algorithm, '--no-sync');
	my @verify = ('pg_verifybackup', '-e', $backup_path);

	# A backup with a bogus algorithm should fail.
	if ($algorithm eq 'bogus')
	{
		$master->command_fails(\@backup,
			"backup fails with algorithm \"$algorithm\"");
		next;
	}

	# A backup with a valid algorithm should work.
	$master->command_ok(\@backup, "backup ok with algorithm \"$algorithm\"");

	# We expect each real checksum algorithm to be mentioned on every line of
	# the backup manifest file except the first and last; for simplicity, we
	# just check that it shows up lots of times. When the checksum algorithm
	# is none, we just check that the manifest exists.
	if ($algorithm eq 'none')
	{
		ok(-f "$backup_path/backup_manifest", "backup manifest exists");
	}
	else
	{
		my $manifest = slurp_file("$backup_path/backup_manifest");
		my $count_of_algorithm_in_manifest =
		  (() = $manifest =~ /$algorithm/mig);
		cmp_ok($count_of_algorithm_in_manifest,
			'>', 100, "$algorithm is mentioned many times in the manifest");
	}

	# Make sure that it verifies OK.
	$master->command_ok(\@verify,
		"verify backup with algorithm \"$algorithm\"");

	# Remove backup immediately to save disk space.
	rmtree($backup_path);
}
