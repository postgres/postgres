
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# This test aims to verify that the client can decompress and extract
# a backup which was compressed by the server.

use strict;
use warnings FATAL => 'all';
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
		'backup_flags' => [ '--compress', 'server-gzip:5' ],
		'enabled' => check_pg_config("#define HAVE_LIBZ 1")
	},
	{
		'compression_method' => 'lz4',
		'backup_flags' => [ '--compress', 'server-lz4:5' ],
		'enabled' => check_pg_config("#define USE_LZ4 1")
	},
	{
		'compression_method' => 'zstd',
		'backup_flags' => [ '--compress', 'server-zstd:5' ],
		'enabled' => check_pg_config("#define USE_ZSTD 1")
	},
	{
		'compression_method' => 'parallel zstd',
		'backup_flags' => [ '--compress', 'server-zstd:workers=3' ],
		'enabled' => check_pg_config("#define USE_ZSTD 1"),
		'possibly_unsupported' =>
		  qr/could not set compression worker count to 3: Unsupported parameter/
	});

for my $tc (@test_configuration)
{
	my $backup_path = $primary->backup_dir . '/' . 'extract_backup';
	my $method = $tc->{'compression_method'};

  SKIP:
	{
		skip "$method compression not supported by this build", 2
		  if !$tc->{'enabled'};

		# Take backup with server compression enabled.
		my @backup = (
			'pg_basebackup', '-D', $backup_path,
			'-Xfetch', '--no-sync', '-cfast', '-Fp');
		push @backup, @{ $tc->{'backup_flags'} };

		my @verify = ('pg_verifybackup', '-e', $backup_path);

		# A backup with a valid compression method should work.
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
			skip "compression with $method not supported by this build", 2;
		}
		else
		{
			ok($backup_result, "backup done, compression $method");
		}

		# Make sure that it verifies OK.
		$primary->command_ok(\@verify,
			"backup verified, compression method \"$method\"");
	}

	# Remove backup immediately to save disk space.
	rmtree($backup_path);
}

done_testing();
