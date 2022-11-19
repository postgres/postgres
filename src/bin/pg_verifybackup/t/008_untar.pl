# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# This test case aims to verify that server-side backups and server-side
# backup compression work properly, and it also aims to verify that
# pg_verifybackup can verify a base backup that didn't start out in plain
# format.

use strict;
use warnings;
use File::Path qw(rmtree);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->start;

my $backup_path  = $primary->backup_dir . '/server-backup';
my $extract_path = $primary->backup_dir . '/extracted-backup';

my @test_configuration = (
	{
		'compression_method' => 'none',
		'backup_flags'       => [],
		'backup_archive'     => 'base.tar',
		'enabled'            => 1
	},
	{
		'compression_method' => 'gzip',
		'backup_flags'       => [ '--compress', 'server-gzip' ],
		'backup_archive'     => 'base.tar.gz',
		'decompress_program' => $ENV{'GZIP_PROGRAM'},
		'decompress_flags'   => ['-d'],
		'enabled'            => check_pg_config("#define HAVE_LIBZ 1")
	},
	{
		'compression_method' => 'lz4',
		'backup_flags'       => [ '--compress', 'server-lz4' ],
		'backup_archive'     => 'base.tar.lz4',
		'decompress_program' => $ENV{'LZ4'},
		'decompress_flags'   => [ '-d', '-m' ],
		'enabled'            => check_pg_config("#define USE_LZ4 1")
	},
	{
		'compression_method' => 'zstd',
		'backup_flags'       => [ '--compress', 'server-zstd' ],
		'backup_archive'     => 'base.tar.zst',
		'decompress_program' => $ENV{'ZSTD'},
		'decompress_flags'   => ['-d'],
		'enabled'            => check_pg_config("#define USE_ZSTD 1")
	});

for my $tc (@test_configuration)
{
	my $method = $tc->{'compression_method'};

  SKIP:
	{
		skip "$method compression not supported by this build", 3
		  if !$tc->{'enabled'};
		skip "no decompressor available for $method", 3
		  if exists $tc->{'decompress_program'}
		  && (!defined $tc->{'decompress_program'}
			|| $tc->{'decompress_program'} eq '');

		# Take a server-side backup.
		my @backup = (
			'pg_basebackup',       '--no-sync',
			'-cfast',              '--target',
			"server:$backup_path", '-Xfetch');
		push @backup, @{ $tc->{'backup_flags'} };
		$primary->command_ok(\@backup,
			"server side backup, compression $method");


		# Verify that the we got the files we expected.
		my $backup_files = join(',',
			sort grep { $_ ne '.' && $_ ne '..' } slurp_dir($backup_path));
		my $expected_backup_files =
		  join(',', sort ('backup_manifest', $tc->{'backup_archive'}));
		is($backup_files, $expected_backup_files,
			"found expected backup files, compression $method");

		# Decompress.
		if (exists $tc->{'decompress_program'})
		{
			my @decompress = ($tc->{'decompress_program'});
			push @decompress, @{ $tc->{'decompress_flags'} }
			  if $tc->{'decompress_flags'};
			push @decompress, $backup_path . '/' . $tc->{'backup_archive'};
			system_or_bail(@decompress);
		}

	  SKIP:
		{
			my $tar = $ENV{TAR};
			# don't check for a working tar here, to accommodate various odd
			# cases such as AIX. If tar doesn't work the init_from_backup below
			# will fail.
			skip "no tar program available", 1
			  if (!defined $tar || $tar eq '');

			# Untar.
			mkdir($extract_path);
			system_or_bail($tar, 'xf', $backup_path . '/base.tar',
				'-C', $extract_path);

			# Verify.
			$primary->command_ok(
				[
					'pg_verifybackup', '-n',
					'-m', "$backup_path/backup_manifest",
					'-e', $extract_path
				],
				"verify backup, compression $method");
		}

		# Cleanup.
		unlink($backup_path . '/backup_manifest');
		unlink($backup_path . '/base.tar');
		unlink($backup_path . '/' . $tc->{'backup_archive'});
		rmtree($extract_path);
	}
}

done_testing();
