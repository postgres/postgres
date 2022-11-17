# TestLib, low-level routines and actions regression tests.
#
# This module contains a set of routines dedicated to environment setup for
# a PostgreSQL regression test run and includes some low-level routines
# aimed at controlling command execution, logging and test functions. This
# module should never depend on any other PostgreSQL regression test modules.

package TestLib;

use strict;
use warnings;

use Config;
use Cwd;
use Exporter 'import';
use Fcntl qw(:mode :seek);
use File::Basename;
use File::Spec;
use File::Temp ();
use IPC::Run;
use SimpleTee;
use Test::More;

our @EXPORT = qw(
  slurp_dir
  slurp_file
  append_to_file
  system_or_bail
  system_log
  run_log

  command_ok
  command_fails
  command_exit_is
  program_help_ok
  program_version_ok
  program_options_handling_ok
  command_like

  $windows_os
);

our ($windows_os, $tmp_check, $log_path, $test_logfile);

BEGIN
{

	# Set to untranslated messages, to be able to compare program output
	# with expected strings.
	delete $ENV{LANGUAGE};
	delete $ENV{LC_ALL};
	$ENV{LC_MESSAGES} = 'C';

	# This list should be kept in sync with pg_regress.c.
	my @envkeys = qw (
	  PGCLIENTENCODING
	  PGCONNECT_TIMEOUT
	  PGDATA
	  PGDATABASE
	  PGGSSLIB
	  PGHOSTADDR
	  PGKRBSRVNAME
	  PGPASSFILE
	  PGPASSWORD
	  PGREQUIREPEER
	  PGREQUIRESSL
	  PGSERVICE
	  PGSERVICEFILE
	  PGSSLCERT
	  PGSSLCRL
	  PGSSLKEY
	  PGSSLMODE
	  PGSSLROOTCERT
	  PGUSER
	  PGPORT
	  PGHOST
	);
	delete @ENV{@envkeys};

	# Must be set early
	$windows_os = $Config{osname} eq 'MSWin32' || $Config{osname} eq 'msys';
	if ($windows_os)
	{
		require Win32API::File;
		Win32API::File->import(qw(createFile OsFHandleOpen CloseHandle));
	}
}

INIT
{

	# Determine output directories, and create them.  The base path is the
	# TESTDIR environment variable, which is normally set by the invoking
	# Makefile.
	$tmp_check = $ENV{TESTDIR} ? "$ENV{TESTDIR}/tmp_check" : "tmp_check";
	$log_path = "$tmp_check/log";

	mkdir $tmp_check;
	mkdir $log_path;

	# Open the test log file, whose name depends on the test name.
	$test_logfile = basename($0);
	$test_logfile =~ s/\.[^.]+$//;
	$test_logfile = "$log_path/regress_log_$test_logfile";
	open TESTLOG, '>', $test_logfile
	  or die "could not open STDOUT to logfile \"$test_logfile\": $!";

	# Hijack STDOUT and STDERR to the log file
	open(ORIG_STDOUT, ">&STDOUT");
	open(ORIG_STDERR, ">&STDERR");
	open(STDOUT,      ">&TESTLOG");
	open(STDERR,      ">&TESTLOG");

	# The test output (ok ...) needs to be printed to the original STDOUT so
	# that the 'prove' program can parse it, and display it to the user in
	# real time. But also copy it to the log file, to provide more context
	# in the log.
	my $builder = Test::More->builder;
	my $fh      = $builder->output;
	tie *$fh, "SimpleTee", *ORIG_STDOUT, *TESTLOG;
	$fh = $builder->failure_output;
	tie *$fh, "SimpleTee", *ORIG_STDERR, *TESTLOG;

	# Enable auto-flushing for all the file handles. Stderr and stdout are
	# redirected to the same file, and buffering causes the lines to appear
	# in the log in confusing order.
	autoflush STDOUT 1;
	autoflush STDERR 1;
	autoflush TESTLOG 1;
}

END
{

	# Test files have several ways of causing prove_check to fail:
	# 1. Exit with a non-zero status.
	# 2. Call ok(0) or similar, indicating that a constituent test failed.
	# 3. Deviate from the planned number of tests.
	#
	# Preserve temporary directories after (1) and after (2).
	$File::Temp::KEEP_ALL = 1 unless $? == 0 && all_tests_passing();
}

sub all_tests_passing
{
	my $fail_count = 0;
	foreach my $status (Test::More->builder->summary)
	{
		return 0 unless $status;
	}
	return 1;
}

#
# Helper functions
#
sub tempdir
{
	my ($prefix) = @_;
	$prefix = "tmp_test" unless defined $prefix;
	return File::Temp::tempdir(
		$prefix . '_XXXX',
		DIR     => $tmp_check,
		CLEANUP => 1);
}

sub tempdir_short
{

	# Use a separate temp dir outside the build tree for the
	# Unix-domain socket, to avoid file name length issues.
	return File::Temp::tempdir(CLEANUP => 1);
}

# Translate a Perl file name to a host file name.  Currently, this is a no-op
# except for the case of Perl=msys and host=mingw32.  The subject need not
# exist, but its parent directory must exist.
sub perl2host
{
	my ($subject) = @_;
	return $subject unless $Config{osname} eq 'msys';
	my $here = cwd;
	my $leaf;
	if (chdir $subject)
	{
		$leaf = '';
	}
	else
	{
		$leaf = '/' . basename $subject;
		my $parent = dirname $subject;
		chdir $parent or die "could not chdir \"$parent\": $!";
	}

	# this odd way of calling 'pwd -W' is the only way that seems to work.
	my $dir = qx{sh -c "pwd -W"};
	chomp $dir;
	chdir $here;
	return $dir . $leaf;
}

sub system_log
{
	print("# Running: " . join(" ", @_) . "\n");
	return system(@_);
}

sub system_or_bail
{
	if (system_log(@_) != 0)
	{
		BAIL_OUT("system $_[0] failed");
	}
}

sub run_log
{
	print("# Running: " . join(" ", @{ $_[0] }) . "\n");
	return IPC::Run::run(@_);
}

sub slurp_dir
{
	my ($dir) = @_;
	opendir(my $dh, $dir)
	  or die "could not opendir \"$dir\": $!";
	my @direntries = readdir $dh;
	closedir $dh;
	return @direntries;
}

sub slurp_file
{
	my ($filename, $offset) = @_;
	local $/;
	my $contents;
	my $fh;

	# On windows open file using win32 APIs, to allow us to set the
	# FILE_SHARE_DELETE flag ("d" below), otherwise other accesses to the file
	# may fail.
	if ($Config{osname} ne 'MSWin32')
	{
		open($fh, '<', $filename)
		  or die "could not read \"$filename\": $!";
	}
	else
	{
		my $fHandle = createFile($filename, "r", "rwd")
		  or die "could not open \"$filename\": $^E";
		OsFHandleOpen($fh = IO::Handle->new(), $fHandle, 'r')
		  or die "could not read \"$filename\": $^E\n";
	}

	if (defined($offset))
	{
		seek($fh, $offset, SEEK_SET)
		  or die "could not seek \"$filename\": $!";
	}

	$contents = <$fh>;
	close $fh;

	$contents =~ s/\r\n/\n/g if $Config{osname} eq 'msys';
	return $contents;
}

sub append_to_file
{
	my ($filename, $str) = @_;
	open my $fh, ">>", $filename
	  or die "could not write \"$filename\": $!";
	print $fh $str;
	close $fh;
}

#
# Test functions
#
sub command_ok
{
	my ($cmd, $test_name) = @_;
	my $result = run_log($cmd);
	ok($result, $test_name);
}

sub command_fails
{
	my ($cmd, $test_name) = @_;
	my $result = run_log($cmd);
	ok(!$result, $test_name);
}

sub command_exit_is
{
	my ($cmd, $expected, $test_name) = @_;
	print("# Running: " . join(" ", @{$cmd}) . "\n");
	my $h = IPC::Run::start $cmd;
	$h->finish();

	# Normally, if the child called exit(N), IPC::Run::result() returns N.  On
	# Windows, with IPC::Run v20220807.0 and earlier, full_results() is the
	# method that returns N (https://github.com/toddr/IPC-Run/issues/161).
	my $result =
	  ($Config{osname} eq "MSWin32" && $IPC::Run::VERSION <= 20220807.0)
	  ? ($h->full_results)[0]
	  : $h->result(0);
	is($result, $expected, $test_name);
}

sub program_help_ok
{
	my ($cmd) = @_;
	my ($stdout, $stderr);
	print("# Running: $cmd --help\n");
	my $result = IPC::Run::run [ $cmd, '--help' ], '>', \$stdout, '2>',
	  \$stderr;
	ok($result, "$cmd --help exit code 0");
	isnt($stdout, '', "$cmd --help goes to stdout");
	is($stderr, '', "$cmd --help nothing to stderr");
}

sub program_version_ok
{
	my ($cmd) = @_;
	my ($stdout, $stderr);
	print("# Running: $cmd --version\n");
	my $result = IPC::Run::run [ $cmd, '--version' ], '>', \$stdout, '2>',
	  \$stderr;
	ok($result, "$cmd --version exit code 0");
	isnt($stdout, '', "$cmd --version goes to stdout");
	is($stderr, '', "$cmd --version nothing to stderr");
}

sub program_options_handling_ok
{
	my ($cmd) = @_;
	my ($stdout, $stderr);
	print("# Running: $cmd --not-a-valid-option\n");
	my $result = IPC::Run::run [ $cmd, '--not-a-valid-option' ], '>',
	  \$stdout,
	  '2>', \$stderr;
	ok(!$result, "$cmd with invalid option nonzero exit code");
	isnt($stderr, '', "$cmd with invalid option prints error message");
}

sub command_like
{
	my ($cmd, $expected_stdout, $test_name) = @_;
	my ($stdout, $stderr);
	print("# Running: " . join(" ", @{$cmd}) . "\n");
	my $result = IPC::Run::run $cmd, '>', \$stdout, '2>', \$stderr;
	ok($result, "@$cmd exit code 0");
	is($stderr, '', "@$cmd no stderr");
	$stdout =~ s/\r\n/\n/g if $Config{osname} eq 'msys';
	like($stdout, $expected_stdout, "$test_name: matches");
}

# Run a command and check its status and outputs.
# The 5 arguments are:
# - cmd: ref to list for command, options and arguments to run
# - ret: expected exit status
# - out: ref to list of re to be checked against stdout (all must match)
# - err: ref to list of re to be checked against stderr (all must match)
# - test_name: name of test
sub command_checks_all
{
	my ($cmd, $expected_ret, $out, $err, $test_name) = @_;

	# run command
	my ($stdout, $stderr);
	print("# Running: " . join(" ", @{$cmd}) . "\n");
	IPC::Run::run($cmd, '>', \$stdout, '2>', \$stderr);

	# See http://perldoc.perl.org/perlvar.html#%24CHILD_ERROR
	my $ret = $?;
	die "command exited with signal " . ($ret & 127)
	  if $ret & 127;
	$ret = $ret >> 8;

	foreach ($stderr, $stdout) { s/\r\n/\n/g if $Config{osname} eq 'msys'; }

	# check status
	ok($ret == $expected_ret,
		"$test_name status (got $ret vs expected $expected_ret)");

	# check stdout
	for my $re (@$out)
	{
		like($stdout, $re, "$test_name stdout /$re/");
	}

	# check stderr
	for my $re (@$err)
	{
		like($stderr, $re, "$test_name stderr /$re/");
	}

	return;
}

1;
