# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# This test case aims to verify that client-side backup compression work
# properly, and it also aims to verify that pg_verifybackup can verify a base
# backup that didn't start out in plain format.

use strict;
use warnings FATAL => 'all';
use File::Path qw(rmtree);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->start;

my $backup_path = $primary->backup_dir . '/client-backup';
my $extract_path = $primary->backup_dir . '/extracted-backup';

my @test_configuration = (
	{
		'compression_method' => 'none',
		'backup_flags' => [],
		'backup_archive' => 'base.tar',
		'enabled' => 1
	},
	{
		'compression_method' => 'gzip',
		'backup_flags' => [ '--compress', 'client-gzip:5' ],
		'backup_archive' => 'base.tar.gz',
		'enabled' => check_pg_config("#define HAVE_LIBZ 1")
	},
	{
		'compression_method' => 'lz4',
		'backup_flags' => [ '--compress', 'client-lz4:5' ],
		'backup_archive' => 'base.tar.lz4',
		'enabled' => check_pg_config("#define USE_LZ4 1")
	},
	{
		'compression_method' => 'zstd',
		'backup_flags' => [ '--compress', 'client-zstd:5' ],
		'backup_archive' => 'base.tar.zst',
		'enabled' => check_pg_config("#define USE_ZSTD 1")
	},
	{
		'compression_method' => 'zstd',
		'backup_flags' => [ '--compress', 'client-zstd:level=1,long' ],
		'backup_archive' => 'base.tar.zst',
		'enabled' => check_pg_config("#define USE_ZSTD 1")
	},
	{
		'compression_method' => 'parallel zstd',
		'backup_flags' => [ '--compress', 'client-zstd:workers=3' ],
		'backup_archive' => 'base.tar.zst',
		'enabled' => check_pg_config("#define USE_ZSTD 1"),
		'possibly_unsupported' =>
		  qr/could not set compression worker count to 3: Unsupported parameter/
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

		# Take a client-side backup.
		my @backup = (
			'pg_basebackup', '-D', $backup_path,
			'-Xfetch', '--no-sync', '-cfast', '-Ft');
		push @backup, @{ $tc->{'backup_flags'} };
		my $backup_stdout = '';
		my $backup_stderr = '';
		my $backup_result = $primary->run_log(\@backup, '>', \$backup_stdout,
			'2>', \$backup_stderr);
		if ($backup_stdout ne '')
		{
			print "# standard output was:\n$backup_stdout";
		}
		if ($backup_stderr ne '')
		{
			print "# standard error was:\n$backup_stderr";
		}
		if (  !$backup_result
			&& $tc->{'possibly_unsupported'}
			&& $backup_stderr =~ /$tc->{'possibly_unsupported'}/)
		{
			skip "compression with $method not supported by this build", 3;
		}
		else
		{
			ok($backup_result, "client side backup, compression $method");
		}

		# Verify that the we got the files we expected.
		my $backup_files = join(',',
			sort grep { $_ ne '.' && $_ ne '..' } slurp_dir($backup_path));
		my $expected_backup_files =
		  join(',', sort ('backup_manifest', $tc->{'backup_archive'}));
		is($backup_files, $expected_backup_files,
			"found expected backup files, compression $method");

		# Verify tar backup.
		$primary->command_ok( [ 'pg_verifybackup', '-n', '-e', $backup_path ],
			"verify backup, compression $method");

		# Cleanup.
		rmtree($extract_path);
		rmtree($backup_path);
	}
}

done_testing();
