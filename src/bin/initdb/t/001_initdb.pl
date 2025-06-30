
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# To test successful data directory creation with an additional feature, first
# try to elaborate the "successful creation" test instead of adding a test.
# Successful initdb consumes much time and I/O.

use strict;
use warnings FATAL => 'all';
use Fcntl ':mode';
use File::stat qw{lstat};
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $tempdir = PostgreSQL::Test::Utils::tempdir;
my $xlogdir = "$tempdir/pgxlog";
my $datadir = "$tempdir/data";
my $supports_syncfs = check_pg_config("#define HAVE_SYNCFS 1");

program_help_ok('initdb');
program_version_ok('initdb');
program_options_handling_ok('initdb');

command_fails([ 'initdb', '--sync-only', "$tempdir/nonexistent" ],
	'sync missing data directory');

mkdir $xlogdir;
mkdir "$xlogdir/lost+found";
command_fails([ 'initdb', '--waldir' => $xlogdir, $datadir ],
	'existing nonempty xlog directory');
rmdir "$xlogdir/lost+found";
command_fails(
	[ 'initdb', '--waldir' => 'pgxlog', $datadir ],
	'relative xlog directory not allowed');

command_fails([ 'initdb', '--username' => 'pg_test', $datadir ],
	'role names cannot begin with "pg_"');

mkdir $datadir;

# make sure we run one successful test without a TZ setting so we test
# initdb's time zone setting code
{

	# delete local only works from perl 5.12, so use the older way to do this
	local (%ENV) = %ENV;
	delete $ENV{TZ};

	# while we are here, also exercise --text-search-config and --set options
	command_ok(
		[
			'initdb', '--no-sync',
			'--text-search-config' => 'german',
			'--set' => 'default_text_search_config=german',
			'--waldir' => $xlogdir,
			$datadir
		],
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

# Control file should tell that data checksums are enabled by default.
command_like(
	[ 'pg_controldata', $datadir ],
	qr/Data page checksum version:.*1/,
	'checksums are enabled in control file');

command_ok([ 'initdb', '--sync-only', $datadir ], 'sync only');
command_ok([ 'initdb', '--sync-only', '--no-sync-data-files', $datadir ],
	'--no-sync-data-files');
command_fails([ 'initdb', $datadir ], 'existing data directory');

if ($supports_syncfs)
{
	command_ok(
		[ 'initdb', '--sync-only', $datadir, '--sync-method' => 'syncfs' ],
		'sync method syncfs');
}
else
{
	command_fails(
		[ 'initdb', '--sync-only', $datadir, '--sync-method' => 'syncfs' ],
		'sync method syncfs');
}

# Check group access on PGDATA
SKIP:
{
	skip "unix-style permissions not supported on Windows", 2
	  if ($windows_os || $Config::Config{osname} eq 'cygwin');

	# Init a new db with group access
	my $datadir_group = "$tempdir/data_group";

	command_ok([ 'initdb', '--allow-group-access', $datadir_group ],
		'successful creation with group access');

	ok(check_mode_recursive($datadir_group, 0750, 0640),
		'check PGDATA permissions');
}

# Locale provider tests

if ($ENV{with_icu} eq 'yes')
{
	command_fails_like(
		[
			'initdb', '--no-sync',
			'--locale-provider' => 'icu',
			"$tempdir/data2"
		],
		qr/initdb: error: locale must be specified if provider is icu/,
		'locale provider ICU requires --icu-locale');

	command_ok(
		[
			'initdb', '--no-sync',
			'--locale-provider' => 'icu',
			'--icu-locale' => 'en',
			"$tempdir/data3"
		],
		'option --icu-locale');

	command_like(
		[
			'initdb', '--no-sync',
			'--auth' => 'trust',
			'--locale-provider' => 'icu',
			'--locale' => 'und',
			'--lc-collate' => 'C',
			'--lc-ctype' => 'C',
			'--lc-messages' => 'C',
			'--lc-numeric' => 'C',
			'--lc-monetary' => 'C',
			'--lc-time' => 'C',
			"$tempdir/data4"
		],
		qr/^\s+default collation:\s+und\n/ms,
		'options --locale-provider=icu --locale=und --lc-*=C');

	command_fails_like(
		[
			'initdb', '--no-sync',
			'--locale-provider' => 'icu',
			'--icu-locale' => '@colNumeric=lower',
			"$tempdir/dataX"
		],
		qr/could not open collator for locale/,
		'fails for invalid ICU locale');

	command_fails_like(
		[
			'initdb', '--no-sync',
			'--locale-provider' => 'icu',
			'--encoding' => 'SQL_ASCII',
			'--icu-locale' => 'en',
			"$tempdir/dataX"
		],
		qr/error: encoding mismatch/,
		'fails for encoding not supported by ICU');

	command_fails_like(
		[
			'initdb', '--no-sync',
			'--locale-provider' => 'icu',
			'--icu-locale' => 'nonsense-nowhere',
			"$tempdir/dataX"
		],
		qr/error: locale "nonsense-nowhere" has unknown language "nonsense"/,
		'fails for nonsense language');

	command_fails_like(
		[
			'initdb', '--no-sync',
			'--locale-provider' => 'icu',
			'--icu-locale' => '@colNumeric=lower',
			"$tempdir/dataX"
		],
		qr/could not open collator for locale "und-u-kn-lower": U_ILLEGAL_ARGUMENT_ERROR/,
		'fails for invalid collation argument');
}
else
{
	command_fails(
		[
			'initdb', '--no-sync',
			'--locale-provider' => 'icu',
			"$tempdir/data2"
		],
		'locale provider ICU fails since no ICU support');
}

command_fails(
	[
		'initdb', '--no-sync',
		'--locale-provider' => 'builtin',
		"$tempdir/data6"
	],
	'locale provider builtin fails without --locale');

command_ok(
	[
		'initdb', '--no-sync',
		'--locale-provider' => 'builtin',
		'--locale' => 'C',
		"$tempdir/data7"
	],
	'locale provider builtin with --locale');

command_ok(
	[
		'initdb', '--no-sync',
		'--locale-provider' => 'builtin',
		'--encoding' => 'UTF-8',
		'--lc-collate' => 'C',
		'--lc-ctype' => 'C',
		'--builtin-locale' => 'C.UTF-8',
		"$tempdir/data8"
	],
	'locale provider builtin with --encoding=UTF-8 --builtin-locale=C.UTF-8');

command_fails(
	[
		'initdb', '--no-sync',
		'--locale-provider' => 'builtin',
		'--encoding' => 'SQL_ASCII',
		'--lc-collate' => 'C',
		'--lc-ctype' => 'C',
		'--builtin-locale' => 'C.UTF-8',
		"$tempdir/data9"
	],
	'locale provider builtin with --builtin-locale=C.UTF-8 fails for SQL_ASCII'
);

command_ok(
	[
		'initdb', '--no-sync',
		'--locale-provider' => 'builtin',
		'--lc-ctype' => 'C',
		'--locale' => 'C',
		"$tempdir/data10"
	],
	'locale provider builtin with --lc-ctype');

command_fails(
	[
		'initdb', '--no-sync',
		'--locale-provider' => 'builtin',
		'--icu-locale' => 'en',
		"$tempdir/dataX"
	],
	'fails for locale provider builtin with ICU locale');

command_fails(
	[
		'initdb', '--no-sync',
		'--locale-provider' => 'builtin',
		'--icu-rules' => '""',
		"$tempdir/dataX"
	],
	'fails for locale provider builtin with ICU rules');

command_fails(
	[
		'initdb', '--no-sync',
		'--locale-provider' => 'xyz',
		"$tempdir/dataX"
	],
	'fails for invalid locale provider');

command_fails(
	[
		'initdb', '--no-sync',
		'--locale-provider' => 'libc',
		'--icu-locale' => 'en',
		"$tempdir/dataX"
	],
	'fails for invalid option combination');

command_fails(
	[
		'initdb', '--no-sync',
		'--set' => 'foo=bar',
		"$tempdir/dataX"
	],
	'fails for invalid --set option');

# Make sure multiple invocations of --set parameters are added case
# insensitive.
command_ok(
	[
		'initdb', '--no-sync',
		'--set' => 'work_mem=128',
		'--set' => 'Work_Mem=256',
		'--set' => 'WORK_MEM=512',
		"$tempdir/dataY"
	],
	'multiple --set options with different case');

my $conf = slurp_file("$tempdir/dataY/postgresql.conf");
ok($conf !~ qr/^WORK_MEM = /m, "WORK_MEM should not be configured");
ok($conf !~ qr/^Work_Mem = /m, "Work_Mem should not be configured");
ok($conf =~ qr/^work_mem = 512/m, "work_mem should be in config");

# Test the no-data-checksums flag
my $datadir_nochecksums = "$tempdir/data_no_checksums";

command_ok([ 'initdb', '--no-data-checksums', $datadir_nochecksums ],
	'successful creation without data checksums');

# Control file should tell that data checksums are disabled.
command_like(
	[ 'pg_controldata', $datadir_nochecksums ],
	qr/Data page checksum version:.*0/,
	'checksums are disabled in control file');

# pg_checksums fails with checksums disabled. This is
# not part of the tests included in pg_checksums to save from
# the creation of an extra instance.
command_fails(
	[ 'pg_checksums', '--pgdata' => $datadir_nochecksums ],
	"pg_checksums fails with data checksum disabled");

done_testing();
