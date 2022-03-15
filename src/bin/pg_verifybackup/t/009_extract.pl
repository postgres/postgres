
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# This test aims to verify that the client can decompress and extract
# a backup which was compressed by the server.

use strict;
use warnings;
use Cwd;
use Config;
use File::Path qw(rmtree);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->start;

my @test_configuration = (
	{
		'compression_method' => 'none',
		'backup_flags' => [],
		'enabled' => 1
	},
	{
		'compression_method' => 'gzip',
		'backup_flags' => ['--compress', 'server-gzip:5'],
		'enabled' => check_pg_config("#define HAVE_LIBZ 1")
	},
	{
		'compression_method' => 'lz4',
		'backup_flags' => ['--compress', 'server-lz4:5'],
		'enabled' => check_pg_config("#define USE_LZ4 1")
	},
	{
		'compression_method' => 'zstd',
		'backup_flags' => ['--compress', 'server-zstd:5'],
		'enabled' => check_pg_config("#define USE_ZSTD 1")
	}
);

for my $tc (@test_configuration)
{
	my $backup_path = $primary->backup_dir . '/' . 'extract_backup';
	my $method = $tc->{'compression_method'};

	SKIP: {
		skip "$method compression not supported by this build", 2
			if ! $tc->{'enabled'};

		# Take backup with server compression enabled.
		my @backup      = (
			'pg_basebackup', '-D', $backup_path,
			'-Xfetch', '--no-sync', '-cfast', '-Fp');
		push @backup, @{$tc->{'backup_flags'}};

		my @verify = ('pg_verifybackup', '-e', $backup_path);

		# A backup with a valid compression method should work.
		$primary->command_ok(\@backup,
							 "backup done, compression method \"$method\"");

		# Make sure that it verifies OK.
		$primary->command_ok(\@verify,
							"backup verified, compression method \"$method\"");
	}

	# Remove backup immediately to save disk space.
	rmtree($backup_path);
}

done_testing();
