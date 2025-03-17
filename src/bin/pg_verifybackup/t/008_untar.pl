# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# This test case aims to verify that server-side backups and server-side
# backup compression work properly, and it also aims to verify that
# pg_verifybackup can verify a base backup that didn't start out in plain
# format.

use strict;
use warnings FATAL => 'all';
use File::Path qw(rmtree);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->start;

# Create a tablespace directory.
my $source_ts_path = PostgreSQL::Test::Utils::tempdir_short();

# Create a tablespace with table in it.
$primary->safe_psql(
	'postgres', qq(
		CREATE TABLESPACE regress_ts1 LOCATION '$source_ts_path';
		SELECT oid FROM pg_tablespace WHERE spcname = 'regress_ts1';
		CREATE TABLE regress_tbl1(i int) TABLESPACE regress_ts1;
		INSERT INTO regress_tbl1 VALUES(generate_series(1,5));));
my $tsoid = $primary->safe_psql(
	'postgres', qq(
		SELECT oid FROM pg_tablespace WHERE spcname = 'regress_ts1'));

my $backup_path = $primary->backup_dir . '/server-backup';
my $extract_path = $primary->backup_dir . '/extracted-backup';

my @test_configuration = (
	{
		'compression_method' => 'none',
		'backup_flags' => [],
		'backup_archive' => [ 'base.tar', "$tsoid.tar" ],
		'enabled' => 1
	},
	{
		'compression_method' => 'gzip',
		'backup_flags' => [ '--compress', 'server-gzip' ],
		'backup_archive' => [ 'base.tar.gz', "$tsoid.tar.gz" ],
		'enabled' => check_pg_config("#define HAVE_LIBZ 1")
	},
	{
		'compression_method' => 'lz4',
		'backup_flags' => [ '--compress', 'server-lz4' ],
		'backup_archive' => [ 'base.tar.lz4', "$tsoid.tar.lz4" ],
		'enabled' => check_pg_config("#define USE_LZ4 1")
	},
	{
		'compression_method' => 'zstd',
		'backup_flags' => [ '--compress', 'server-zstd' ],
		'backup_archive' => [ 'base.tar.zst', "$tsoid.tar.zst" ],
		'enabled' => check_pg_config("#define USE_ZSTD 1")
	},
	{
		'compression_method' => 'zstd',
		'backup_flags' => [ '--compress', 'server-zstd:level=1,long' ],
		'backup_archive' => [ 'base.tar.zst', "$tsoid.tar.zst" ],
		'enabled' => check_pg_config("#define USE_ZSTD 1")
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
		$primary->command_ok(
			[
				'pg_basebackup', '--no-sync',
				'--checkpoint' => 'fast',
				'--target' => "server:$backup_path",
				'--wal-method' => 'fetch',
				@{ $tc->{'backup_flags'} },
			],
			"server side backup, compression $method");


		# Verify that the we got the files we expected.
		my $backup_files = join(',',
			sort grep { $_ ne '.' && $_ ne '..' } slurp_dir($backup_path));
		my $expected_backup_files =
		  join(',', sort ('backup_manifest', @{ $tc->{'backup_archive'} }));
		is($backup_files, $expected_backup_files,
			"found expected backup files, compression $method");

		# Verify tar backup.
		$primary->command_ok(
			[
				'pg_verifybackup', '--no-parse-wal',
				'--exit-on-error', $backup_path,
			],
			"verify backup, compression $method");

		# Cleanup.
		rmtree($backup_path);
		rmtree($extract_path);
	}
}

done_testing();
