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
use File::Find;
use File::Spec;
use File::stat qw(stat);
use File::Temp ();
use IPC::Run;
use SimpleTee;

# specify a recent enough version of Test::More to support the done_testing() function
use Test::More 0.87;

our @EXPORT = qw(
  generate_ascii_string
  slurp_dir
  slurp_file
  append_to_file
  check_mode_recursive
  chmod_recursive
  check_pg_config
  scan_server_header
  system_or_bail
  system_log
  run_log
  run_command
  pump_until

  command_ok
  command_fails
  command_exit_is
  program_help_ok
  program_version_ok
  program_options_handling_ok
  command_like
  command_like_safe
  command_fails_like
  command_checks_all

  $windows_os
);

our ($windows_os, $timeout_default, $tmp_check, $log_path, $test_logfile);

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
	  PGGSSENCMODE
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
	  PGTARGETSESSIONATTRS
	  PGUSER
	  PGPORT
	  PGHOST
	  PG_COLOR
	);
	delete @ENV{@envkeys};

	$ENV{PGAPPNAME} = basename($0);

	# Must be set early
	$windows_os = $Config{osname} eq 'MSWin32' || $Config{osname} eq 'msys';
	if ($windows_os)
	{
		require Win32API::File;
		Win32API::File->import(qw(createFile OsFHandleOpen CloseHandle));
	}

	$timeout_default = $ENV{PG_TEST_TIMEOUT_DEFAULT};
	$timeout_default = 180
	  if not defined $timeout_default or $timeout_default eq '';
}

INIT
{

	# Return EPIPE instead of killing the process with SIGPIPE.  An affected
	# test may still fail, but it's more likely to report useful facts.
	$SIG{PIPE} = 'IGNORE';

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
	open my $testlog, '>', $test_logfile
	  or die "could not open STDOUT to logfile \"$test_logfile\": $!";

	# Hijack STDOUT and STDERR to the log file
	open(my $orig_stdout, '>&', \*STDOUT);
	open(my $orig_stderr, '>&', \*STDERR);
	open(STDOUT,          '>&', $testlog);
	open(STDERR,          '>&', $testlog);

	# The test output (ok ...) needs to be printed to the original STDOUT so
	# that the 'prove' program can parse it, and display it to the user in
	# real time. But also copy it to the log file, to provide more context
	# in the log.
	my $builder = Test::More->builder;
	my $fh      = $builder->output;
	tie *$fh, "SimpleTee", $orig_stdout, $testlog;
	$fh = $builder->failure_output;
	tie *$fh, "SimpleTee", $orig_stderr, $testlog;

	# Enable auto-flushing for all the file handles. Stderr and stdout are
	# redirected to the same file, and buffering causes the lines to appear
	# in the log in confusing order.
	autoflush STDOUT 1;
	autoflush STDERR 1;
	autoflush $testlog 1;
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
		CLEANUP => not defined $ENV{'PG_TEST_NOCLEAN'});
}

sub tempdir_short
{

	# Use a separate temp dir outside the build tree for the
	# Unix-domain socket, to avoid file name length issues.
	return File::Temp::tempdir(
		CLEANUP => not defined $ENV{'PG_TEST_NOCLEAN'});
}

=pod

=item has_wal_read_bug()

Returns true if $tmp_check is subject to a sparc64+ext4 bug that causes WAL
readers to see zeros if another process simultaneously wrote the same offsets.
Consult this in tests that fail frequently on affected configurations.  The
bug has made streaming standbys fail to advance, reporting corrupt WAL.  It
has made COMMIT PREPARED fail with "could not read two-phase state from WAL".
Non-WAL PostgreSQL reads haven't been affected, likely because those readers
and writers have buffering systems in common.  See
https://postgr.es/m/20220116210241.GC756210@rfd.leadboat.com for details.

=cut

sub has_wal_read_bug
{
	return
	     $Config{osname} eq 'linux'
	  && $Config{archname} =~ /^sparc/
	  && !run_log([ qw(df -x ext4), $tmp_check ], '>', '/dev/null', '2>&1');
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
	return;
}

sub run_log
{
	print("# Running: " . join(" ", @{ $_[0] }) . "\n");
	return IPC::Run::run(@_);
}

sub run_command
{
	my ($cmd) = @_;
	my ($stdout, $stderr);
	my $result = IPC::Run::run $cmd, '>', \$stdout, '2>', \$stderr;
	chomp($stdout);
	chomp($stderr);
	return ($stdout, $stderr);
}

=pod

=item pump_until(proc, timeout, stream, until)

Pump until string is matched on the specified stream, or timeout occurs.

=cut

sub pump_until
{
	my ($proc, $timeout, $stream, $until) = @_;
	$proc->pump_nb();
	while (1)
	{
		last if $$stream =~ /$until/;
		if ($timeout->is_expired)
		{
			diag("pump_until: timeout expired when searching for \"$until\" with stream: \"$$stream\"");
			return 0;
		}
		if (not $proc->pumpable())
		{
			diag("pump_until: process terminated unexpectedly when searching for \"$until\" with stream: \"$$stream\"");
			return 0;
		}
		$proc->pump();
	}
	return 1;
}

# Generate a string made of the given range of ASCII characters
sub generate_ascii_string
{
	my ($from_char, $to_char) = @_;
	my $res;

	for my $i ($from_char .. $to_char)
	{
		$res .= sprintf("%c", $i);
	}
	return $res;
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

	return $contents;
}

sub append_to_file
{
	my ($filename, $str) = @_;
	open my $fh, ">>", $filename
	  or die "could not write \"$filename\": $!";
	print $fh $str;
	close $fh;
	return;
}

# Check that all file/dir modes in a directory match the expected values,
# ignoring the mode of any specified files.
sub check_mode_recursive
{
	my ($dir, $expected_dir_mode, $expected_file_mode, $ignore_list) = @_;

	# Result defaults to true
	my $result = 1;

	find(
		{
			follow_fast => 1,
			wanted      => sub {
				# Is file in the ignore list?
				foreach my $ignore ($ignore_list ? @{$ignore_list} : [])
				{
					if ("$dir/$ignore" eq $File::Find::name)
					{
						return;
					}
				}

				# Allow ENOENT.  A running server can delete files, such as
				# those in pg_stat.  Other stat() failures are fatal.
				my $file_stat = stat($File::Find::name);
				unless (defined($file_stat))
				{
					my $is_ENOENT = $!{ENOENT};
					my $msg       = "unable to stat $File::Find::name: $!";
					if ($is_ENOENT)
					{
						warn $msg;
						return;
					}
					else
					{
						die $msg;
					}
				}

				my $file_mode = S_IMODE($file_stat->mode);

				# Is this a file?
				if (S_ISREG($file_stat->mode))
				{
					if ($file_mode != $expected_file_mode)
					{
						print(
							*STDERR,
							sprintf("$File::Find::name mode must be %04o\n",
								$expected_file_mode));

						$result = 0;
						return;
					}
				}

				# Else a directory?
				elsif (S_ISDIR($file_stat->mode))
				{
					if ($file_mode != $expected_dir_mode)
					{
						print(
							*STDERR,
							sprintf("$File::Find::name mode must be %04o\n",
								$expected_dir_mode));

						$result = 0;
						return;
					}
				}

				# Else something we can't handle
				else
				{
					die "unknown file type for $File::Find::name";
				}
			}
		},
		$dir);

	return $result;
}

# Change mode recursively on a directory
sub chmod_recursive
{
	my ($dir, $dir_mode, $file_mode) = @_;

	find(
		{
			follow_fast => 1,
			wanted      => sub {
				my $file_stat = stat($File::Find::name);

				if (defined($file_stat))
				{
					chmod(
						S_ISDIR($file_stat->mode) ? $dir_mode : $file_mode,
						$File::Find::name
					) or die "unable to chmod $File::Find::name";
				}
			}
		},
		$dir);
	return;
}

# Returns an array that stores all the matches of the given regular expression
# within the PostgreSQL installation's C<header_path>.  This can be used to
# retrieve specific value patterns from the installation's header files.
sub scan_server_header
{
	my ($header_path, $regexp) = @_;

	my ($stdout, $stderr);
	my $result = IPC::Run::run [ 'pg_config', '--includedir-server' ], '>',
	  \$stdout, '2>', \$stderr
	  or die "could not execute pg_config";
	chomp($stdout);
	$stdout =~ s/\r$//;

	open my $header_h, '<', "$stdout/$header_path" or die "$!";

	my @match = undef;
	while (<$header_h>)
	{
		my $line = $_;

		if (@match = $line =~ /^$regexp/)
		{
			last;
		}
	}

	close $header_h;
	die "could not find match in header $header_path\n"
	  unless @match;
	return @match;
}

# Check presence of a given regexp within pg_config.h for the installation
# where tests are running, returning a match status result depending on
# that.
sub check_pg_config
{
	my ($regexp) = @_;
	my ($stdout, $stderr);
	my $result = IPC::Run::run [ 'pg_config', '--includedir' ], '>',
	  \$stdout, '2>', \$stderr
	  or die "could not execute pg_config";
	chomp($stdout);
	$stdout =~ s/\r$//;

	open my $pg_config_h, '<', "$stdout/pg_config.h" or die "$!";
	my $match = (grep { /^$regexp/ } <$pg_config_h>);
	close $pg_config_h;
	return $match;
}

#
# Test functions
#
sub command_ok
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;
	my ($cmd, $test_name) = @_;
	my $result = run_log($cmd);
	ok($result, $test_name);
	return;
}

sub command_fails
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;
	my ($cmd, $test_name) = @_;
	my $result = run_log($cmd);
	ok(!$result, $test_name);
	return;
}

sub command_exit_is
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;
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
	return;
}

sub program_help_ok
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;
	my ($cmd) = @_;
	my ($stdout, $stderr);
	print("# Running: $cmd --help\n");
	my $result = IPC::Run::run [ $cmd, '--help' ], '>', \$stdout, '2>',
	  \$stderr;
	ok($result, "$cmd --help exit code 0");
	isnt($stdout, '', "$cmd --help goes to stdout");
	is($stderr, '', "$cmd --help nothing to stderr");
	return;
}

sub program_version_ok
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;
	my ($cmd) = @_;
	my ($stdout, $stderr);
	print("# Running: $cmd --version\n");
	my $result = IPC::Run::run [ $cmd, '--version' ], '>', \$stdout, '2>',
	  \$stderr;
	ok($result, "$cmd --version exit code 0");
	isnt($stdout, '', "$cmd --version goes to stdout");
	is($stderr, '', "$cmd --version nothing to stderr");
	return;
}

sub program_options_handling_ok
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;
	my ($cmd) = @_;
	my ($stdout, $stderr);
	print("# Running: $cmd --not-a-valid-option\n");
	my $result = IPC::Run::run [ $cmd, '--not-a-valid-option' ], '>',
	  \$stdout,
	  '2>', \$stderr;
	ok(!$result, "$cmd with invalid option nonzero exit code");
	isnt($stderr, '', "$cmd with invalid option prints error message");
	return;
}

sub command_like
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;
	my ($cmd, $expected_stdout, $test_name) = @_;
	my ($stdout, $stderr);
	print("# Running: " . join(" ", @{$cmd}) . "\n");
	my $result = IPC::Run::run $cmd, '>', \$stdout, '2>', \$stderr;
	ok($result, "$test_name: exit code 0");
	is($stderr, '', "$test_name: no stderr");
	like($stdout, $expected_stdout, "$test_name: matches");
	return;
}

sub command_like_safe
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	# Doesn't rely on detecting end of file on the file descriptors,
	# which can fail, causing the process to hang, notably on Msys
	# when used with 'pg_ctl start'
	my ($cmd, $expected_stdout, $test_name) = @_;
	my ($stdout, $stderr);
	my $stdoutfile = File::Temp->new();
	my $stderrfile = File::Temp->new();
	print("# Running: " . join(" ", @{$cmd}) . "\n");
	my $result = IPC::Run::run $cmd, '>', $stdoutfile, '2>', $stderrfile;
	$stdout = slurp_file($stdoutfile);
	$stderr = slurp_file($stderrfile);
	ok($result, "$test_name: exit code 0");
	is($stderr, '', "$test_name: no stderr");
	like($stdout, $expected_stdout, "$test_name: matches");
	return;
}

sub command_fails_like
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;
	my ($cmd, $expected_stderr, $test_name) = @_;
	my ($stdout, $stderr);
	print("# Running: " . join(" ", @{$cmd}) . "\n");
	my $result = IPC::Run::run $cmd, '>', \$stdout, '2>', \$stderr;
	ok(!$result, "$test_name: exit code not 0");
	like($stderr, $expected_stderr, "$test_name: matches");
	return;
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
	local $Test::Builder::Level = $Test::Builder::Level + 1;

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
