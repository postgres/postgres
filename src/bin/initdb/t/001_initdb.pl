
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# To test successful data directory creation with an additional feature, first
# try to elaborate the "successful creation" test instead of adding a test.
# Successful initdb consumes much time and I/O.

use strict;
use warnings;
use Fcntl ':mode';
use File::stat qw{lstat};
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $tempdir = PostgreSQL::Test::Utils::tempdir;
my $xlogdir = "$tempdir/pgxlog";
my $datadir = "$tempdir/data";

program_help_ok('initdb');
program_version_ok('initdb');
program_options_handling_ok('initdb');

command_fails([ 'initdb', '-S', "$tempdir/nonexistent" ],
	'sync missing data directory');

mkdir $xlogdir;
mkdir "$xlogdir/lost+found";
command_fails(
	[ 'initdb', '-X', $xlogdir, $datadir ],
	'existing nonempty xlog directory');
rmdir "$xlogdir/lost+found";
command_fails(
	[ 'initdb', '-X', 'pgxlog', $datadir ],
	'relative xlog directory not allowed');

command_fails(
	[ 'initdb', '-U', 'pg_test', $datadir ],
	'role names cannot begin with "pg_"');

mkdir $datadir;

# make sure we run one successful test without a TZ setting so we test
# initdb's time zone setting code
{

	# delete local only works from perl 5.12, so use the older way to do this
	local (%ENV) = %ENV;
	delete $ENV{TZ};

	command_ok([ 'initdb', '-N', '-T', 'german', '-X', $xlogdir, $datadir ],
		'successful creation');

	# Permissions on PGDATA should be default
  SKIP:
	{
		skip "unix-style permissions not supported on Windows", 1
		  if ($windows_os);

		ok(check_mode_recursive($datadir, 0700, 0600),
			"check PGDATA permissions");
	}
}

# Control file should tell that data checksums are disabled by default.
command_like(
	[ 'pg_controldata', $datadir ],
	qr/Data page checksum version:.*0/,
	'checksums are disabled in control file');
# pg_checksums fails with checksums disabled by default.  This is
# not part of the tests included in pg_checksums to save from
# the creation of an extra instance.
command_fails([ 'pg_checksums', '-D', $datadir ],
	"pg_checksums fails with data checksum disabled");

command_ok([ 'initdb', '-S', $datadir ], 'sync only');
command_fails([ 'initdb', $datadir ], 'existing data directory');

# Check group access on PGDATA
SKIP:
{
	skip "unix-style permissions not supported on Windows", 2
	  if ($windows_os);

	# Init a new db with group access
	my $datadir_group = "$tempdir/data_group";

	command_ok(
		[ 'initdb', '-g', $datadir_group ],
		'successful creation with group access');

	ok(check_mode_recursive($datadir_group, 0750, 0640),
		'check PGDATA permissions');
}

# Locale provider tests

if ($ENV{with_icu} eq 'yes')
{
	command_fails_like(
		[ 'initdb', '--no-sync', '--locale-provider=icu', "$tempdir/data2" ],
		qr/initdb: error: ICU locale must be specified/,
		'locale provider ICU requires --icu-locale');

	command_ok(
		[
			'initdb',                '--no-sync',
			'--locale-provider=icu', '--icu-locale=en',
			"$tempdir/data3"
		],
		'option --icu-locale');

	command_fails_like(
		[
			'initdb',                '--no-sync',
			'--locale-provider=icu', '--icu-locale=@colNumeric=lower',
			"$tempdir/dataX"
		],
		qr/FATAL:  could not open collator for locale/,
		'fails for invalid ICU locale');

	command_fails_like(
		[
			'initdb',                '--no-sync',
			'--locale-provider=icu', '--encoding=SQL_ASCII',
			'--icu-locale=en', "$tempdir/dataX"
		],
		qr/error: encoding mismatch/,
		'fails for encoding not supported by ICU');
}
else
{
	command_fails(
		[ 'initdb', '--no-sync', '--locale-provider=icu', "$tempdir/data2" ],
		'locale provider ICU fails since no ICU support');
}

command_fails(
	[ 'initdb', '--no-sync', '--locale-provider=xyz', "$tempdir/dataX" ],
	'fails for invalid locale provider');

command_fails(
	[
		'initdb',                 '--no-sync',
		'--locale-provider=libc', '--icu-locale=en',
		"$tempdir/dataX"
	],
	'fails for invalid option combination');

done_testing();
