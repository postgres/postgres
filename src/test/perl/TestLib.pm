package TestLib;

use strict;
use warnings;

use Exporter 'import';
our @EXPORT = qw(
  tempdir
  tempdir_short
  start_test_server
  restart_test_server
  psql
  system_or_bail

  command_ok
  command_fails
  command_exit_is
  program_help_ok
  program_version_ok
  program_options_handling_ok
  command_like
  issues_sql_like
);

use Cwd;
use File::Spec;
use File::Temp ();
use Test::More;

BEGIN
{
	eval {
		require IPC::Run;
		import IPC::Run qw(run start);
		1;
	} or do
	{
		plan skip_all => "IPC::Run not available";
	};

	eval {
		Test::More->VERSION('0.93_01');
	} or do
	{
		plan skip_all => "version of Test::More is too old to support subplans";
	};

	eval {
		require Test::Simple;
		Test::Simple->VERSION('0.98');
	} or do
	{
		plan skip_all => "version of Test::Simple is too old to support subplans properly";
	};
}

# Set to untranslated messages, to be able to compare program output
# with expected strings.
delete $ENV{LANGUAGE};
delete $ENV{LC_ALL};
$ENV{LC_MESSAGES} = 'C';

delete $ENV{PGCONNECT_TIMEOUT};
delete $ENV{PGDATA};
delete $ENV{PGDATABASE};
delete $ENV{PGHOSTADDR};
delete $ENV{PGREQUIRESSL};
delete $ENV{PGSERVICE};
delete $ENV{PGSSLMODE};
delete $ENV{PGUSER};

if (!$ENV{PGPORT})
{
	$ENV{PGPORT} = 65432;
}

$ENV{PGPORT} = int($ENV{PGPORT}) % 65536;


#
# Helper functions
#


sub tempdir
{
	return File::Temp::tempdir('tmp_testXXXX', DIR => $ENV{TESTDIR} || cwd(), CLEANUP => 1);
}

sub tempdir_short
{
	# Use a separate temp dir outside the build tree for the
	# Unix-domain socket, to avoid file name length issues.
	return File::Temp::tempdir(CLEANUP => 1);
}

my ($test_server_datadir, $test_server_logfile);

sub start_test_server
{
	my ($tempdir) = @_;
	my $ret;

	my $tempdir_short = tempdir_short;

	system "initdb -D '$tempdir'/pgdata -A trust -N >/dev/null";
	$ret = system 'pg_ctl', '-D', "$tempdir/pgdata", '-s', '-w', '-l',
	  "$tempdir/logfile", '-o',
	  "--fsync=off -k $tempdir_short --listen-addresses='' --log-statement=all",
	  'start';

	if ($ret != 0)
	{
		system('cat', "$tempdir/logfile");
		BAIL_OUT("pg_ctl failed");
	}

	$ENV{PGHOST}         = $tempdir_short;
	$test_server_datadir = "$tempdir/pgdata";
	$test_server_logfile = "$tempdir/logfile";
}

sub restart_test_server
{
	system 'pg_ctl', '-s', '-D', $test_server_datadir, '-w', '-l',
	  $test_server_logfile, 'restart';
}

END
{
	if ($test_server_datadir)
	{
		system 'pg_ctl', '-D', $test_server_datadir, '-s', '-w', '-m',
		  'immediate', 'stop';
	}
}

sub psql
{
	my ($dbname, $sql) = @_;
	run [ 'psql', '-X', '-q', '-d', $dbname, '-f', '-' ], '<', \$sql or die;
}

sub system_or_bail
{
	system(@_) == 0 or BAIL_OUT("system @_ failed: $?");
}


#
# Test functions
#


sub command_ok
{
	my ($cmd, $test_name) = @_;
	my $result = run $cmd, '>', File::Spec->devnull(), '2>',
	  File::Spec->devnull();
	ok($result, $test_name);
}

sub command_fails
{
	my ($cmd, $test_name) = @_;
	my $result = run $cmd, '>', File::Spec->devnull(), '2>',
	  File::Spec->devnull();
	ok(!$result, $test_name);
}

sub command_exit_is
{
	my ($cmd, $expected, $test_name) = @_;
	my $h = start $cmd, '>', File::Spec->devnull(), '2>',
	  File::Spec->devnull();
	$h->finish();
	is($h->result(0), $expected, $test_name);
}

sub program_help_ok
{
	my ($cmd) = @_;
	subtest "$cmd --help" => sub {
		plan tests => 3;
		my ($stdout, $stderr);
		my $result = run [ $cmd, '--help' ], '>', \$stdout, '2>', \$stderr;
		ok($result, "$cmd --help exit code 0");
		isnt($stdout, '', "$cmd --help goes to stdout");
		is($stderr, '', "$cmd --help nothing to stderr");
	};
}

sub program_version_ok
{
	my ($cmd) = @_;
	subtest "$cmd --version" => sub {
		plan tests => 3;
		my ($stdout, $stderr);
		my $result = run [ $cmd, '--version' ], '>', \$stdout, '2>', \$stderr;
		ok($result, "$cmd --version exit code 0");
		isnt($stdout, '', "$cmd --version goes to stdout");
		is($stderr, '', "$cmd --version nothing to stderr");
	};
}

sub program_options_handling_ok
{
	my ($cmd) = @_;
	subtest "$cmd options handling" => sub {
		plan tests => 2;
		my ($stdout, $stderr);
		my $result = run [ $cmd, '--not-a-valid-option' ], '>', \$stdout,
		  '2>', \$stderr;
		ok(!$result, "$cmd with invalid option nonzero exit code");
		isnt($stderr, '', "$cmd with invalid option prints error message");
	};
}

sub command_like
{
	my ($cmd, $expected_stdout, $test_name) = @_;
	subtest $test_name => sub {
		plan tests => 3;
		my ($stdout, $stderr);
		my $result = run $cmd, '>', \$stdout, '2>', \$stderr;
		ok($result, "@$cmd exit code 0");
		is($stderr, '', "@$cmd no stderr");
		like($stdout, $expected_stdout, "$test_name: matches");
	};
}

sub issues_sql_like
{
	my ($cmd, $expected_sql, $test_name) = @_;
	subtest $test_name => sub {
		plan tests => 2;
		my ($stdout, $stderr);
		truncate $test_server_logfile, 0;
		my $result = run $cmd, '>', \$stdout, '2>', \$stderr;
		ok($result, "@$cmd exit code 0");
		my $log = `cat '$test_server_logfile'`;
		like($log, $expected_sql, "$test_name: SQL found in server log");
	};
}

1;
