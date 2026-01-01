
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

###############################################################
# This test script uses essentially the same structure as
# 002_pg_dump.pl, but is specialized to deal with compression
# concerns.  As such, some of the test cases here are large
# and would contribute undue amounts of runtime if they were
# included in 002_pg_dump.pl.
###############################################################

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $tempdir = PostgreSQL::Test::Utils::tempdir;

###############################################################
# Definition of the pg_dump runs to make.
#
# In addition to the facilities explained in 002_pg_dump.pl,
# these entries can include:
#
# compile_option indicates if the test depends on a compilation
# option, if any.  This can be used to control if tests should be
# skipped when a build dependency is not satisfied.
#
# compress_cmd is the utility command for (de)compression, if any.
# Note that this should generally be used on pg_dump's output
# either to generate a text file to run the through the tests, or
# to test pg_restore's ability to parse manually compressed files
# that otherwise pg_dump does not compress on its own (e.g. *.toc).

my $supports_gzip = check_pg_config("#define HAVE_LIBZ 1");
my $supports_lz4 = check_pg_config("#define USE_LZ4 1");
my $supports_zstd = check_pg_config("#define USE_ZSTD 1");

my %pgdump_runs = (
	compression_none_custom => {
		test_key => 'compression',
		dump_cmd => [
			'pg_dump', '--no-sync',
			'--format' => 'custom',
			'--compress' => 'none',
			'--file' => "$tempdir/compression_none_custom.dump",
			'--statistics',
			'postgres',
		],
		restore_cmd => [
			'pg_restore',
			'--file' => "$tempdir/compression_none_custom.sql",
			'--statistics',
			"$tempdir/compression_none_custom.dump",
		],
	},

	compression_gzip_custom => {
		test_key => 'compression',
		compile_option => 'gzip',
		dump_cmd => [
			'pg_dump', '--no-sync',
			'--format' => 'custom',
			'--compress' => '1',
			'--file' => "$tempdir/compression_gzip_custom.dump",
			'--statistics',
			'postgres',
		],
		restore_cmd => [
			'pg_restore',
			'--file' => "$tempdir/compression_gzip_custom.sql",
			'--statistics',
			"$tempdir/compression_gzip_custom.dump",
		],
		command_like => {
			command => [
				'pg_restore', '--list',
				"$tempdir/compression_gzip_custom.dump",
			],
			expected => qr/Compression: gzip/,
			name => 'data content is gzip-compressed'
		},
	},

	compression_gzip_dir => {
		test_key => 'compression',
		compile_option => 'gzip',
		dump_cmd => [
			'pg_dump', '--no-sync',
			'--jobs' => '2',
			'--format' => 'directory',
			'--compress' => 'gzip:1',
			'--file' => "$tempdir/compression_gzip_dir",
			'--statistics',
			'postgres',
		],
		# Give coverage for manually-compressed TOC files during restore.
		compress_cmd => {
			program => $ENV{'GZIP_PROGRAM'},
			args => [
				'-f',
				"$tempdir/compression_gzip_dir/toc.dat",
				"$tempdir/compression_gzip_dir/blobs_*.toc",
			],
		},
		# Verify that TOC and data files were compressed
		glob_patterns => [
			"$tempdir/compression_gzip_dir/toc.dat.gz",
			"$tempdir/compression_gzip_dir/*.dat.gz",
		],
		restore_cmd => [
			'pg_restore',
			'--jobs' => '2',
			'--file' => "$tempdir/compression_gzip_dir.sql",
			'--statistics',
			"$tempdir/compression_gzip_dir",
		],
	},

	compression_gzip_plain => {
		test_key => 'compression',
		compile_option => 'gzip',
		dump_cmd => [
			'pg_dump', '--no-sync',
			'--format' => 'plain',
			'--compress' => '1',
			'--file' => "$tempdir/compression_gzip_plain.sql.gz",
			'--statistics',
			'postgres',
		],
		# Decompress the generated file to run through the tests.
		compress_cmd => {
			program => $ENV{'GZIP_PROGRAM'},
			args => [ '-d', "$tempdir/compression_gzip_plain.sql.gz", ],
		},
	},

	compression_lz4_custom => {
		test_key => 'compression',
		compile_option => 'lz4',
		dump_cmd => [
			'pg_dump', '--no-sync',
			'--format' => 'custom',
			'--compress' => 'lz4',
			'--file' => "$tempdir/compression_lz4_custom.dump",
			'--statistics',
			'postgres',
		],
		restore_cmd => [
			'pg_restore',
			'--file' => "$tempdir/compression_lz4_custom.sql",
			'--statistics',
			"$tempdir/compression_lz4_custom.dump",
		],
		command_like => {
			command => [
				'pg_restore', '--list',
				"$tempdir/compression_lz4_custom.dump",
			],
			expected => qr/Compression: lz4/,
			name => 'data content is lz4 compressed'
		},
	},

	compression_lz4_dir => {
		test_key => 'compression',
		compile_option => 'lz4',
		dump_cmd => [
			'pg_dump', '--no-sync',
			'--jobs' => '2',
			'--format' => 'directory',
			'--compress' => 'lz4:1',
			'--file' => "$tempdir/compression_lz4_dir",
			'--statistics',
			'postgres',
		],
		# Give coverage for manually-compressed TOC files during restore.
		compress_cmd => {
			program => $ENV{'LZ4'},
			args => [
				'-z', '-f', '-m', '--rm',
				"$tempdir/compression_lz4_dir/toc.dat",
				"$tempdir/compression_lz4_dir/blobs_*.toc",
			],
		},
		# Verify that TOC and data files were compressed
		glob_patterns => [
			"$tempdir/compression_lz4_dir/toc.dat.lz4",
			"$tempdir/compression_lz4_dir/*.dat.lz4",
		],
		restore_cmd => [
			'pg_restore',
			'--jobs' => '2',
			'--file' => "$tempdir/compression_lz4_dir.sql",
			'--statistics',
			"$tempdir/compression_lz4_dir",
		],
	},

	compression_lz4_plain => {
		test_key => 'compression',
		compile_option => 'lz4',
		dump_cmd => [
			'pg_dump', '--no-sync',
			'--format' => 'plain',
			'--compress' => 'lz4',
			'--file' => "$tempdir/compression_lz4_plain.sql.lz4",
			'--statistics',
			'postgres',
		],
		# Decompress the generated file to run through the tests.
		compress_cmd => {
			program => $ENV{'LZ4'},
			args => [
				'-d', '-f',
				"$tempdir/compression_lz4_plain.sql.lz4",
				"$tempdir/compression_lz4_plain.sql",
			],
		},
	},

	compression_zstd_custom => {
		test_key => 'compression',
		compile_option => 'zstd',
		dump_cmd => [
			'pg_dump', '--no-sync',
			'--format' => 'custom',
			'--compress' => 'zstd',
			'--file' => "$tempdir/compression_zstd_custom.dump",
			'--statistics',
			'postgres',
		],
		restore_cmd => [
			'pg_restore',
			'--file' => "$tempdir/compression_zstd_custom.sql",
			'--statistics',
			"$tempdir/compression_zstd_custom.dump",
		],
		command_like => {
			command => [
				'pg_restore', '--list',
				"$tempdir/compression_zstd_custom.dump",
			],
			expected => qr/Compression: zstd/,
			name => 'data content is zstd compressed'
		},
	},

	compression_zstd_dir => {
		test_key => 'compression',
		compile_option => 'zstd',
		dump_cmd => [
			'pg_dump', '--no-sync',
			'--jobs' => '2',
			'--format' => 'directory',
			'--compress' => 'zstd:1',
			'--file' => "$tempdir/compression_zstd_dir",
			'--statistics',
			'postgres',
		],
		# Give coverage for manually-compressed TOC files during restore.
		compress_cmd => {
			program => $ENV{'ZSTD'},
			args => [
				'-z', '-f', '--rm',
				"$tempdir/compression_zstd_dir/toc.dat",
				"$tempdir/compression_zstd_dir/blobs_*.toc",
			],
		},
		# Verify that TOC and data files were compressed
		glob_patterns => [
			"$tempdir/compression_zstd_dir/toc.dat.zst",
			"$tempdir/compression_zstd_dir/*.dat.zst",
		],
		restore_cmd => [
			'pg_restore',
			'--jobs' => '2',
			'--file' => "$tempdir/compression_zstd_dir.sql",
			'--statistics',
			"$tempdir/compression_zstd_dir",
		],
	},

	# Exercise long mode for test coverage
	compression_zstd_plain => {
		test_key => 'compression',
		compile_option => 'zstd',
		dump_cmd => [
			'pg_dump', '--no-sync',
			'--format' => 'plain',
			'--compress' => 'zstd:long',
			'--file' => "$tempdir/compression_zstd_plain.sql.zst",
			'--statistics',
			'postgres',
		],
		# Decompress the generated file to run through the tests.
		compress_cmd => {
			program => $ENV{'ZSTD'},
			args => [
				'-d', '-f',
				"$tempdir/compression_zstd_plain.sql.zst", "-o",
				"$tempdir/compression_zstd_plain.sql",
			],
		},
	},);

###############################################################
# Definition of the tests to run.
#
# In addition to the facilities explained in 002_pg_dump.pl,
# these entries can include:
#
# compile_option indicates if the test depends on a compilation
# option, if any.  This can be used to control if tests should be
# skipped when a build dependency is not satisfied.

# Tests which are considered 'full' dumps by pg_dump, but there
# are flags used to exclude specific items (ACLs, LOs, etc).
my %full_runs = (compression => 1,);

# This is where the actual tests are defined.
my %tests = (
	'CREATE MATERIALIZED VIEW matview_compression_lz4' => {
		create_order => 20,
		create_sql => 'CREATE MATERIALIZED VIEW
						   matview_compression_lz4 (col2) AS
						   SELECT repeat(\'xyzzy\', 10000);
						   ALTER MATERIALIZED VIEW matview_compression_lz4
						   ALTER COLUMN col2 SET COMPRESSION lz4;',
		regexp => qr/^
			\QCREATE MATERIALIZED VIEW public.matview_compression_lz4 AS\E
			\n\s+\QSELECT repeat('xyzzy'::text, 10000) AS col2\E
			\n\s+\QWITH NO DATA;\E
			.*
			\QALTER TABLE ONLY public.matview_compression_lz4 ALTER COLUMN col2 SET COMPRESSION lz4;\E\n
			/xms,
		compile_option => 'lz4',
		like => {%full_runs},
	},

	'CREATE TABLE test_compression_method' => {
		create_order => 110,
		create_sql => 'CREATE TABLE test_compression_method (
						   col1 text
					   );',
		regexp => qr/^
			\QCREATE TABLE public.test_compression_method (\E\n
			\s+\Qcol1 text\E\n
			\Q);\E
			/xm,
		like => { %full_runs, },
	},

	# Insert enough data to surpass DEFAULT_IO_BUFFER_SIZE during
	# (de)compression operations.  The weird regex is because Perl
	# restricts us to repeat counts of less than 32K.
	'COPY test_compression_method' => {
		create_order => 111,
		create_sql => 'INSERT INTO test_compression_method (col1) '
		  . 'SELECT string_agg(a::text, \'\') FROM generate_series(1,65536) a;',
		regexp => qr/^
			\QCOPY public.test_compression_method (col1) FROM stdin;\E
			\n(?:(?:\d\d\d\d\d\d\d\d\d\d){31657}\d\d\d\d\n){1}\\\.\n
			/xm,
		like => { %full_runs, },
	},

	'CREATE TABLE test_compression' => {
		create_order => 3,
		create_sql => 'CREATE TABLE test_compression (
						   col1 int,
						   col2 text COMPRESSION lz4
					   );',
		regexp => qr/^
			\QCREATE TABLE public.test_compression (\E\n
			\s+\Qcol1 integer,\E\n
			\s+\Qcol2 text\E\n
			\);\n
			.*
			\QALTER TABLE ONLY public.test_compression ALTER COLUMN col2 SET COMPRESSION lz4;\E\n
			/xms,
		compile_option => 'lz4',
		like => {%full_runs},
	},

	# Create a large object so we can test compression of blobs.toc
	'LO create (using lo_from_bytea)' => {
		create_order => 50,
		create_sql =>
		  'SELECT pg_catalog.lo_from_bytea(0, \'\\x310a320a330a340a350a360a370a380a390a\');',
		regexp => qr/^SELECT pg_catalog\.lo_create\('\d+'\);/m,
		like => { %full_runs, },
	},

	'LO load (using lo_from_bytea)' => {
		regexp => qr/^
			\QSELECT pg_catalog.lo_open\E \('\d+',\ \d+\);\n
			\QSELECT pg_catalog.lowrite(0, \E
			\Q'\x310a320a330a340a350a360a370a380a390a');\E\n
			\QSELECT pg_catalog.lo_close(0);\E
			/xm,
		like => { %full_runs, },
	},);

#########################################
# Create a PG instance to test actually dumping from

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

my $port = $node->port;

#########################################
# Set up schemas, tables, etc, to be dumped.

# Build up the create statements
my %create_sql = ();

foreach my $test (
	sort {
		if ($tests{$a}->{create_order} and $tests{$b}->{create_order})
		{
			$tests{$a}->{create_order} <=> $tests{$b}->{create_order};
		}
		elsif ($tests{$a}->{create_order})
		{
			-1;
		}
		elsif ($tests{$b}->{create_order})
		{
			1;
		}
		else
		{
			0;
		}
	} keys %tests)
{
	my $test_db = 'postgres';

	if (defined($tests{$test}->{database}))
	{
		$test_db = $tests{$test}->{database};
	}

	# Skip tests that require an unsupported compile option
	if ($tests{$test}->{compile_option}
		&& (($tests{$test}->{compile_option} eq 'gzip' && !$supports_gzip)
			|| ($tests{$test}->{compile_option} eq 'lz4'
				&& !$supports_lz4)
			|| ($tests{$test}->{compile_option} eq 'zstd'
				&& !$supports_zstd)))
	{
		next;
	}

	if ($tests{$test}->{create_sql})
	{
		# Normalize command ending: strip all line endings, add
		# semicolon if missing, add two newlines.
		my $create_sql = $tests{$test}->{create_sql};
		chomp $create_sql;
		$create_sql .= ';' unless substr($create_sql, -1) eq ';';
		$create_sql{$test_db} .= $create_sql . "\n\n";
	}
}

# Send the combined set of commands to psql
foreach my $db (sort keys %create_sql)
{
	$node->safe_psql($db, $create_sql{$db});
}

#########################################
# Run all runs

foreach my $run (sort keys %pgdump_runs)
{
	my $test_key = $run;
	my $run_db = 'postgres';

	# Skip runs that require an unsupported compile option
	if ($pgdump_runs{$run}->{compile_option}
		&& (($pgdump_runs{$run}->{compile_option} eq 'gzip'
				&& !$supports_gzip)
			|| ($pgdump_runs{$run}->{compile_option} eq 'lz4'
				&& !$supports_lz4)
			|| ($pgdump_runs{$run}->{compile_option} eq 'zstd'
				&& !$supports_zstd)))
	{
		note
		  "$run: skipped due to no $pgdump_runs{$run}->{compile_option} support";
		next;
	}

	$node->command_ok(\@{ $pgdump_runs{$run}->{dump_cmd} },
		"$run: pg_dump runs");

	if ($pgdump_runs{$run}->{compress_cmd})
	{
		my ($compress_cmd) = $pgdump_runs{$run}->{compress_cmd};
		my $compress_program = $compress_cmd->{program};

		# Skip the rest of the test if the compression program is
		# not defined.
		next if (!defined($compress_program) || $compress_program eq '');

		# Arguments may require globbing.
		my @full_compress_cmd = ($compress_program);
		foreach my $arg (@{ $compress_cmd->{args} })
		{
			push @full_compress_cmd, glob($arg);
		}

		command_ok(\@full_compress_cmd, "$run: compression commands");
	}

	if ($pgdump_runs{$run}->{glob_patterns})
	{
		my $glob_patterns = $pgdump_runs{$run}->{glob_patterns};
		foreach my $glob_pattern (@{$glob_patterns})
		{
			my @glob_output = glob($glob_pattern);
			my $ok = 0;
			# certainly found some files if glob() returned multiple matches
			$ok = 1 if (scalar(@glob_output) > 1);
			# if just one match, we need to check if it's real
			$ok = 1 if (scalar(@glob_output) == 1 && -f $glob_output[0]);
			is($ok, 1, "$run: glob check for $glob_pattern");
		}
	}

	if ($pgdump_runs{$run}->{command_like})
	{
		my $cmd_like = $pgdump_runs{$run}->{command_like};
		$node->command_like(
			\@{ $cmd_like->{command} },
			$cmd_like->{expected},
			"$run: " . $cmd_like->{name});
	}

	if ($pgdump_runs{$run}->{restore_cmd})
	{
		$node->command_ok(\@{ $pgdump_runs{$run}->{restore_cmd} },
			"$run: pg_restore runs");
	}

	if ($pgdump_runs{$run}->{test_key})
	{
		$test_key = $pgdump_runs{$run}->{test_key};
	}

	my $output_file = slurp_file("$tempdir/${run}.sql");

	#########################################
	# Run all tests where this run is included
	# as either a 'like' or 'unlike' test.

	foreach my $test (sort keys %tests)
	{
		my $test_db = 'postgres';

		if (defined($pgdump_runs{$run}->{database}))
		{
			$run_db = $pgdump_runs{$run}->{database};
		}

		if (defined($tests{$test}->{database}))
		{
			$test_db = $tests{$test}->{database};
		}

		# Check for proper test definitions
		#
		# Either "all_runs" should be set or there should be a "like" list,
		# even if it is empty.  (This makes the test more self-documenting.)
		if (   !defined($tests{$test}->{all_runs})
			&& !defined($tests{$test}->{like}))
		{
			die "missing \"like\" in test \"$test\"";
		}
		# Check for useless entries in "unlike" list.  Runs that are
		# not listed in "like" don't need to be excluded in "unlike".
		if ($tests{$test}->{unlike}->{$test_key}
			&& !defined($tests{$test}->{like}->{$test_key}))
		{
			die "useless \"unlike\" entry \"$test_key\" in test \"$test\"";
		}

		# Skip tests that require an unsupported compile option
		if ($tests{$test}->{compile_option}
			&& (($tests{$test}->{compile_option} eq 'gzip' && !$supports_gzip)
				|| ($tests{$test}->{compile_option} eq 'lz4'
					&& !$supports_lz4)
				|| ($tests{$test}->{compile_option} eq 'zstd'
					&& !$supports_zstd)))
		{
			next;
		}

		if ($run_db ne $test_db)
		{
			next;
		}

		# Run the test if all_runs is set or if listed as a like, unless it is
		# specifically noted as an unlike (generally due to an explicit
		# exclusion or similar).
		if (($tests{$test}->{like}->{$test_key} || $tests{$test}->{all_runs})
			&& !defined($tests{$test}->{unlike}->{$test_key}))
		{
			if (!like(
					$output_file, $tests{$test}->{regexp},
					"$run: should dump $test"))
			{
				diag("Review $run results in $tempdir");
			}
		}
		else
		{
			if (!unlike(
					$output_file, $tests{$test}->{regexp},
					"$run: should not dump $test"))
			{
				diag("Review $run results in $tempdir");
			}
		}
	}
}

#########################################
# Stop the database instance, which will be removed at the end of the tests.

$node->stop('fast');

done_testing();
